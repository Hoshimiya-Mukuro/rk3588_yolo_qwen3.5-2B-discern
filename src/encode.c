#include "encode.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rk_mpi.h"
#include "mpp_buffer.h"
#include "rk_mpi_cmd.h"
#include "mpp_frame.h"
#include "mpp_packet.h"
#include "mpp_buffer.h"

#include "decode.h"
#include "image_drawing.h"
#include "test_time.h"
#include "yolo.h"

#define MPP_ALIGN(x, a)  (((x) + ((a) - 1)) & ~((a) - 1))

/* 注意：这里曾经自己补过一个 MppEncCodecCfg 结构体（因为随附头文件里没有）。
 * 它的内存布局与 MPP 内部预期不一致 → MPP_ENC_SET_CODEC_CFG 里的 h264:level 等
 * 配置全部静默失效，配合废弃的 SET_PREP_CFG/SET_RC_CFG，分辨率也按不进去：
 * 编码器沿用默认 1280x720 + Level 3.1，而我们喂 1920x1080 数据
 * → 每行 stride 错位累积 → 画面细密水平条纹 + 垂直分割线（花屏）。
 * 现已统一改为 MppEncCfg key-value API（MPP_ENC_SET_CFG），不再需要该结构体。 */

static int g_enc_type;
static MppCtx g_mpp_ctx = NULL;
static MppApi *g_mpp_mpi = NULL;
static MppBufferGroup g_buf_grp = NULL;

static uint8_t *g_nv12_buf = NULL;      /* 带 stride 的 NV12，喂给 MPP */
static size_t g_nv12_size = 0;
static uint8_t *g_packed_buf = NULL;    /* 紧凑 NV12（width*height*3/2），turbojpeg 解码输出 */
static size_t g_packed_size = 0;
static int g_hor_stride;        
static int g_ver_stride;         

static uint8_t g_out_buf[5 * 1024 * 1024];  
static EncoderSpsPps g_sps_pps = {0};            

static int g_width;              
static int g_height;             
static int g_fps;

static void extract_sps_pps(MppPacket packet)
{
    uint8_t *data = (uint8_t *)mpp_packet_get_data(packet);
    size_t len = mpp_packet_get_length(packet);

    const uint8_t START[4] = {0,0,0,1};
    int nal_offsets[3] = {0};   
    int nal_count = 0;

    for (size_t i = 0; i + 3 < len && nal_count < 3; i++) 
    {
        if (memcmp(data + i, START, 4) == 0) 
        {
            nal_offsets[nal_count++] = (int)i;
            i += 3;  
        }
    }

    for (int i = 0; i < nal_count; i++) 
    {
        int start = nal_offsets[i] + 4;                
        int end = (i + 1 < nal_count) ? nal_offsets[i + 1] : (int)len;
        int nal_len = end - start;
        int nal_type = data[start] & 0x1F;

        if (nal_type == 7 && nal_len < (int)sizeof(g_sps_pps.sps)) 
        {
            memcpy(g_sps_pps.sps, data + start, nal_len);
            g_sps_pps.sps_len = nal_len;
        } 
        else if (nal_type == 8 && nal_len < (int)sizeof(g_sps_pps.pps)) 
        {
            memcpy(g_sps_pps.pps, data + start, nal_len);
            g_sps_pps.pps_len = nal_len;
        }
    }

    if (g_sps_pps.sps_len && g_sps_pps.pps_len) 
    {
        g_sps_pps.valid = true;

        /* SPS 前 4 字节 = nal_type / profile_idc / constraint_flags / level_idc
         *   level_idc: 0x1f=31 → Level 3.1（上限 720p，1080p 必花屏）
         *              0x28=40 → Level 4.0
         *              0x32=50 → Level 5.0
         * 只在 SPS 首次拿到或内容变化时打印，避免每个 IDR 都刷屏。 */
        static uint8_t  last_sps[sizeof(g_sps_pps.sps)];
        static uint32_t last_sps_len = 0;
        bool changed = (last_sps_len != g_sps_pps.sps_len) ||
                       (memcmp(last_sps, g_sps_pps.sps, g_sps_pps.sps_len) != 0);
        if (changed)
        {
            last_sps_len = g_sps_pps.sps_len;
            memcpy(last_sps, g_sps_pps.sps, g_sps_pps.sps_len);

            printf("[ENC] SPS=%u bytes, PPS=%u bytes\n", g_sps_pps.sps_len, g_sps_pps.pps_len);
            printf("[ENC] SPS hex:");
            for (uint32_t i = 0; i < g_sps_pps.sps_len && i < 12; i++)
                printf(" %02x", g_sps_pps.sps[i]);
            printf("\n");

            if (g_sps_pps.sps_len >= 4)
            {
                printf("[ENC] profile_idc=%u level_idc=%u\n",
                       g_sps_pps.sps[1], g_sps_pps.sps[3]);
                if (g_sps_pps.sps[3] < 40)
                {
                    printf("[ENC] WARNING: level_idc=%u < 40(Level4.0)，"
                           "编码器配置没生效，1080p 会花屏\n", g_sps_pps.sps[3]);
                }
            }
        }
    }
}

static void yuyv_to_nv12(const uint8_t *yuyv, uint8_t *nv12, int width, int height, int hor_stride)
{
    uint8_t *y  = nv12;
    uint8_t *uv = nv12 + hor_stride * height;

    for (int j = 0; j < height; j++) 
    {
        for (int i = 0; i < width; i += 2) 
        {
            
            int in  = (j * width + i) * 2;           
            int y0  = j * hor_stride + i;            
            int y1  = j * hor_stride + i + 1;        

            y[y0] = yuyv[in];      
            y[y1] = yuyv[in + 2];  

            
            if (j % 2 == 0) 
            {
                int uv_idx = (j / 2) * hor_stride + i;
                uv[uv_idx]     = yuyv[in + 1]; 
                uv[uv_idx + 1] = yuyv[in + 3];  
            }
        }
    }
}



/* 紧凑 NV12 → 带 stride 的 NV12。
 * turbojpeg 输出的是紧凑布局：Y[width*height] 后紧跟 UV[width*height/2]；
 * 而 MPP 要求 UV 平面落在 hor_stride*ver_stride 处。1080p 时 ver_stride=1088≠1080，
 * 整块 memcpy 会让色度整体错位 8 行（偏色 + 底部彩噪）。这里按行重排。
 * 前提：g_nv12_buf 已清零，padding（列/行对齐区）全程保持 0。 */
static void packed_to_strided(const uint8_t *src, uint8_t *dst,
                              int width, int height, int hor_stride, int ver_stride)
{
    const uint8_t *src_y  = src;
    const uint8_t *src_uv = src + (size_t)width * height;
    uint8_t *dst_y  = dst;
    uint8_t *dst_uv = dst + (size_t)hor_stride * ver_stride;

    for (int j = 0; j < height; j++)
    {
        memcpy(dst_y + (size_t)j * hor_stride, src_y + (size_t)j * width, (size_t)width);
    }
    for (int j = 0; j < height / 2; j++)
    {
        memcpy(dst_uv + (size_t)j * hor_stride, src_uv + (size_t)j * width, (size_t)width);
    }
}

static int preemcode_sps_pps(void)
{
    memset(g_nv12_buf, 0, g_hor_stride * g_ver_stride);
    memset(g_nv12_buf + g_hor_stride * g_ver_stride, 128, g_hor_stride * g_ver_stride / 2);

    MppFrame frame = NULL;
    MppPacket packet = NULL;
    MppBuffer buf = NULL;
    int ret = -1;

    mpp_frame_init(&frame);
    mpp_frame_set_width(frame, g_width);
    mpp_frame_set_height(frame, g_height);
    mpp_frame_set_hor_stride(frame, g_hor_stride);
    mpp_frame_set_ver_stride(frame, g_ver_stride);
    mpp_frame_set_hor_stride_pixel(frame, g_width);
    mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
    mpp_frame_set_eos(frame, 0);
    mpp_frame_set_pts(frame, 0);
    mpp_frame_set_dts(frame, 0);

    if (mpp_buffer_get(g_buf_grp, &buf, g_nv12_size) != MPP_OK)
    {
        goto out;
    }
    memcpy(mpp_buffer_get_ptr(buf), g_nv12_buf, g_nv12_size);
    __sync_synchronize();
    mpp_frame_set_buffer(frame, buf);

    if (g_mpp_mpi->encode_put_frame(g_mpp_ctx, frame) != MPP_OK)
    {
        printf("[ENC] pre-encode put_frame failed\n");
        goto out;
    }

    if (g_mpp_mpi->poll(g_mpp_ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK) < 0)
    {
        printf("[ENC] pre-encode poll failed\n");
        goto out;
    }

    if (g_mpp_mpi->encode_get_packet(g_mpp_ctx, &packet) != MPP_OK || !packet)
    {
        printf("[ENC] pre-encode get_packet failed\n");
        goto out;
    }
    if (mpp_packet_get_length(packet) > 0)
    {
        extract_sps_pps(packet);
    }
    ret = 0;

out:
    if (packet) mpp_packet_deinit(&packet);
    if (frame)  mpp_frame_deinit(&frame);
    if (buf)    mpp_buffer_put(buf);
    return ret;
}

int encoder_init(int type, int width, int height, int fps, int bitrate, int gop)
{
    g_enc_type = type;
    g_width = width;
    g_height = height;
    g_fps = fps;

    if(type == ENC_MJPEG)
    {
        printf("[ENC] MJPEG passthrough (no init needed)\n");
        return 0;
    }
    if(type == ENC_MJPEG_SW)
    {
        printf("[ENC] YUYV→MJPEG via libjpeg (not implemented yet)\n");
        return -1;
    }

    g_hor_stride = MPP_ALIGN(width, 16);
    g_ver_stride = MPP_ALIGN(height, 16);

    g_nv12_size = g_hor_stride * g_ver_stride * 3 / 2;
    g_nv12_buf  = (uint8_t *)malloc(g_nv12_size);
    if (!g_nv12_buf)
    {
        printf("[ENC] malloc NV12 buf failed\n");
        return -1;
    }
    /* stride 对齐产生的列/行 padding 全程保持 0，不要把这些垃圾喂给编码器 */
    memset(g_nv12_buf, 0, g_nv12_size);

    g_packed_size = (size_t)width * height * 3 / 2;
    g_packed_buf  = (uint8_t *)malloc(g_packed_size);
    if (!g_packed_buf)
    {
        printf("[ENC] malloc packed NV12 buf failed\n");
        goto fail_nv12;
    }

    /* ── 初始化 TurboJPEG 解码器（复用 handle，避免每帧 init/destroy） ── */
    if (decode_init() != 0)
    {
        printf("[ENC] decode_init failed\n");
        goto fail_nv12;
    }

    /* 注：原先这里还残留过一份从未生效的 MppEncCfg（"input:format" = YUV422P），
     * 既没有用 MPP_ENC_SET_CFG 应用，也没有 mpp_enc_cfg_deinit（内存泄漏），已删除。
     * 编码器输入格式现在由下面的 MppEncCfg "prep:format"（YUV420SP）统一决定。 */

    /* ── 1. mpp_create + mpp_init（顺序重要！新版 MPP 要求 init 在 control 之前）── */
    MPP_RET ret = mpp_create(&g_mpp_ctx, &g_mpp_mpi);
    if(ret != MPP_OK)
    {
        printf("[ENC] mpp_create failed (ret=%d)\n", ret);
        goto fail_nv12;
    }

    ret = mpp_init(g_mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
    if (ret != MPP_OK)
    {
        printf("[ENC] mpp_init failed (ret=%d)\n", ret);
        mpp_destroy(g_mpp_ctx);
        g_mpp_ctx = NULL;
        g_mpp_mpi = NULL;
        goto fail_nv12;
    }

    /* ── 2~4. 统一用 MppEncCfg key-value API 配置（新版 API）──
     * MPP_ENC_SET_PREP_CFG / SET_RC_CFG / SET_CODEC_CFG 三个 struct 接口已废弃，
     * 在 RK3588 上会静默失效（分辨率/stride/level 都按不进去），
     * 编码器会沿用默认的 1280x720 + Level 3.1，喂 1080p 数据必然花屏。 */
    MppEncCfg mcfg;
    mpp_enc_cfg_init(&mcfg);

    /* 先读当前配置，再覆盖要改的项 */
    if (g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_GET_CFG, mcfg))
    {
        printf("[ENC] get cfg failed\n");
        mpp_enc_cfg_deinit(mcfg);
        goto fail_mpp;
    }

    /* prep: 输入分辨率与 stride */
    mpp_enc_cfg_set_s32(mcfg, "prep:width",      width);
    mpp_enc_cfg_set_s32(mcfg, "prep:height",     height);
    mpp_enc_cfg_set_s32(mcfg, "prep:hor_stride", g_hor_stride);
    mpp_enc_cfg_set_s32(mcfg, "prep:ver_stride", g_ver_stride);
    mpp_enc_cfg_set_s32(mcfg, "prep:format",     MPP_FMT_YUV420SP);

    /* rc: 码率控制 */
    mpp_enc_cfg_set_s32(mcfg, "rc:mode",         0);
    mpp_enc_cfg_set_s32(mcfg, "rc:bps_target",   bitrate);
    mpp_enc_cfg_set_s32(mcfg, "rc:bps_max",      bitrate * 6 / 5);
    mpp_enc_cfg_set_s32(mcfg, "rc:bps_min",      bitrate * 4 / 5);
    mpp_enc_cfg_set_s32(mcfg, "rc:fps_in_num",   fps);
    mpp_enc_cfg_set_s32(mcfg, "rc:fps_in_denom", 1);
    mpp_enc_cfg_set_s32(mcfg, "rc:fps_out_num",  fps);
    mpp_enc_cfg_set_s32(mcfg, "rc:fps_out_denom", 1);
    mpp_enc_cfg_set_s32(mcfg, "rc:gop",          gop);
    mpp_enc_cfg_set_s32(mcfg, "rc:qp_init",      26);
    mpp_enc_cfg_set_s32(mcfg, "rc:skip_cnt",     0);

    /* codec: H.264 参数（level 必须够高，1080p 至少 Level 4.0） */
    mpp_enc_cfg_set_s32(mcfg, "codec:type",               MPP_VIDEO_CodingAVC);
    mpp_enc_cfg_set_s32(mcfg, "h264:profile",             66);   /* Baseline */
    mpp_enc_cfg_set_s32(mcfg, "h264:level",               50);   /* Level 5.0 */
    mpp_enc_cfg_set_s32(mcfg, "h264:entropy_coding_mode", 1);
    mpp_enc_cfg_set_s32(mcfg, "h264:deblock_disable",     0);

    if (g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_CFG, mcfg))
    {
        printf("[ENC] set cfg failed\n");
        mpp_enc_cfg_deinit(mcfg);
        goto fail_mpp;
    }
    mpp_enc_cfg_deinit(mcfg);

    printf("[ENC] MppEncCfg OK: %dx%d stride=%dx%d level=50\n",
           width, height, g_hor_stride, g_ver_stride);

    /* ── 5. 其他设置 ── */
    MppEncSliceSplit split_cfg;
    ret = g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_GET_SPLIT, &split_cfg);
    if (!ret)
    {
        split_cfg.split_mode = MPP_ENC_SPLIT_NONE;
        split_cfg.split_arg  = 0;
        split_cfg.split_out  = 0;
        g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_SPLIT, &split_cfg);
    }

    MppEncHeaderMode header_mode = MPP_ENC_HEADER_MODE_EACH_IDR;
    g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_HEADER_MODE, &header_mode);

    MppEncSeiMode sei_mode = MPP_ENC_SEI_MODE_ONE_SEQ;
    g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_SEI_CFG, &sei_mode);

    /* ── 6. 创建 buffer group ── */
    ret = mpp_buffer_group_get_internal(&g_buf_grp, MPP_BUFFER_TYPE_DRM);
    if (ret)
    {
        printf("[ENC] DRM buffer group failed (ret=%d)\n", ret);
        goto fail_mpp;
    }
    /* count=0 不设上限 — 编码完成后会正确释放，不会撑爆 */
    mpp_buffer_group_limit_config(g_buf_grp, 0, 0);

    /* ── 7. 预编码一帧获取 SPS/PPS ── */
    if(preemcode_sps_pps() != 0)
    {
        printf("[ENC] pre-encode SPS/PPS failed (non-fatal)\n");
    }

    /* ── 8. 强制下一帧为 IDR（预编码吃掉了第一个 IDR 槽位，不加 VLC 看不到画面）── */
    {
        RK_S32 force_idr = 1;
        g_mpp_mpi->control(g_mpp_ctx, MPP_ENC_SET_IDR_FRAME, &force_idr);
    }

    printf("[ENC] MPP H.264 encoder init OK (%dx%d@%d, %d kbps)\n", width, height, fps, bitrate / 1000);
    return 0;

fail_mpp:
    decode_destroy();
    if (g_mpp_ctx) {
        mpp_destroy(g_mpp_ctx);
        g_mpp_ctx = NULL;
        g_mpp_mpi = NULL;
    }
fail_nv12:
    free(g_nv12_buf);
    g_nv12_buf = NULL;
    free(g_packed_buf);
    g_packed_buf = NULL;
    g_packed_size = 0;
    return -1;
}

/* ── 检测框叠加：把最近一次检测结果画到紧凑 NV12 上 ── */
#define OVL_CLS_ID 0             /* 只画 person（COCO 0） */
#define OVL_CONF   0.50f         /* 只画置信度高于此的框，避免虚警闪烁 */
#define OVL_THICK  4             /* 边框粗细(px)，需为偶数 */
#define OVL_FONT   24            /* 标签字号，需为偶数 */
#define OVL_COLOR  0xFF00FF00u   /* ARGB 纯绿 */

static void encoder_draw_overlay(uint8_t *nv12, int width, int height)
{
    yolo_result_t res;
    char text[64];

    if (yolo_snapshot(&res) != 1 || res.count <= 0)
    {
        return;
    }
    if (res.frame_w != width || res.frame_h != height)
    {
        return;   /* 分辨率变化时旧坐标不可用 */
    }

    image_buffer_t img;
    memset(&img, 0, sizeof(img));
    img.width = width;
    img.height = height;
    img.format = IMAGE_FORMAT_YUV420SP_NV12;
    img.virt_addr = nv12;
    img.size = width * height * 3 / 2;

    for (int i = 0; i < res.count; i++)
    {
        if (res.objects[i].cls_id != OVL_CLS_ID)
        {
            continue;
        }
        if (res.objects[i].prop < OVL_CONF)
        {
            continue;
        }

        int x = res.objects[i].left;
        int y = res.objects[i].top;
        int rw = res.objects[i].right - x;
        int rh = res.objects[i].bottom - y;
        if (rw <= 0 || rh <= 0)
        {
            continue;
        }

        /* RGA 无关，但绘制库的 UV 平面按 x/2 取整，圆整到偶数更准 */
        x &= ~1;
        y &= ~1;
        rw &= ~1;
        rh &= ~1;

        draw_rectangle(&img, x, y, rw, rh, OVL_COLOR, OVL_THICK);

        snprintf(text, sizeof(text), "%s %.2f",
                 yolo_cls_to_name(res.objects[i].cls_id), res.objects[i].prop);
        int ty = y - OVL_FONT * 2;
        if (ty < 0)
        {
            ty = y + 2;   /* 框贴顶时画到框内上沿 */
        }
        draw_text(&img, text, x, ty, OVL_COLOR, OVL_FONT);
    }
}

int encoder_encode(const void *in, size_t in_len, void **out, size_t *out_len, int *is_key, int *is_sps_pps, uint64_t pts)
{
    *out = NULL;
    *out_len = 0;
    *is_key = 0;
    *is_sps_pps = 0;

    MppFrame frame = NULL;
    MppPacket packet = NULL;
    MppBuffer buf = NULL;
    int frame_sent = 0;
    int ret = -1;
    
    if(g_enc_type == ENC_MJPEG)//MJPEG
    {
        if(in_len > sizeof(g_out_buf))
        {
            printf("[ENC] MJPEG frame too large: %zu\n", in_len);
            return -1;
        }
        memcpy(g_out_buf, in, in_len);
        *out = g_out_buf;
        *out_len = in_len;
        *is_key = 1;
        return 0;
    }
    else if(g_enc_type == ENC_MPP)//MPP
    {
        if(!g_mpp_mpi)
        {
            return -1;
        }

        LOG_TIME("enc: mjpeg_to_nv12 start");
        if (mjpeg_to_nv12((const uint8_t *)in, in_len, g_packed_buf, g_width, g_height) != 0)
        {
            printf("[ENC] mjpeg_to_nv12 decode failed\n");
            return -1;
        }
        LOG_TIME("enc: mjpeg_to_nv12 end");

        /* ── AI 旁路：紧凑 NV12 生成后立刻投递给识别线程 ──
         * 非阻塞：识别线程忙时内部直接丢帧，不会拖慢采集/推流。
         * g_packed_buf 是紧凑 NV12（stride == width），正是 yolo 需要的格式。 */
        yolo_publish_frame(g_packed_buf, g_width, g_height);

        /* ── 检测框叠加：只改送 MPP 的这一份，VLM 拿到的是上面投递的干净帧 ──
         * 用"最近一次"检测结果，避免把 YOLO 的 22ms 塞进编码链路。 */
        encoder_draw_overlay(g_packed_buf, g_width, g_height);

        LOG_TIME("enc: buf_prep start");
        /* 紧凑 NV12 → MPP 要求的 stride 布局（1080p 必须，否则色度错位） */
        packed_to_strided(g_packed_buf, g_nv12_buf, g_width, g_height, g_hor_stride, g_ver_stride);
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, g_width);
        mpp_frame_set_height(frame, g_height);
        mpp_frame_set_hor_stride(frame, g_hor_stride);
        mpp_frame_set_ver_stride(frame, g_ver_stride);
        mpp_frame_set_hor_stride_pixel(frame, g_width);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_eos(frame, 0);
        mpp_frame_set_pts(frame, (RK_S64)pts);
        mpp_frame_set_dts(frame, (RK_S64)pts);

        if(mpp_buffer_get(g_buf_grp, &buf, g_nv12_size) != MPP_OK)
        {
            printf("[ENC] buffer get failed\n");
            goto out;
        }
        memcpy(mpp_buffer_get_ptr(buf), g_nv12_buf, g_nv12_size);
        mpp_frame_set_buffer(frame, buf);
        LOG_TIME("enc: buf_prep end");

        /* ── async split API: put_frame → poll → get_packet ── */
        LOG_TIME("enc: MPP encode start");
        ret = g_mpp_mpi->encode_put_frame(g_mpp_ctx, frame);
        if (ret != MPP_OK)
        {
            printf("[ENC] put_frame failed (ret=%d)\n", ret);
            goto out;
        }
        frame_sent = 1;

        ret = g_mpp_mpi->poll(g_mpp_ctx, MPP_PORT_OUTPUT, MPP_POLL_BLOCK);
        if (ret < 0)
        {
            printf("[ENC] poll failed (ret=%d)\n", ret);
            goto out;
        }
        LOG_TIME("enc: MPP encode end");

        LOG_TIME("enc: get_packet start");
        ret = g_mpp_mpi->encode_get_packet(g_mpp_ctx, &packet);
        if (ret != MPP_OK || !packet || mpp_packet_get_length(packet) == 0)
        {
            goto out;
        }
        LOG_TIME("enc: get_packet end");

        /* ── 编码完成，释放 frame（回收 buffer 到 pool，不会撑爆） ── */
        mpp_frame_deinit(&frame);
        mpp_buffer_put(buf);
        frame = NULL;  buf = NULL;
        frame_sent = 0;

        uint8_t *pkt_data = (uint8_t *)mpp_packet_get_data(packet);
        size_t pkt_len = mpp_packet_get_length(packet);

        if (pkt_len > 4 && pkt_data[0] == 0 && pkt_data[1] == 0 && pkt_data[2] == 0 && pkt_data[3] == 1)
        {
            int nal = pkt_data[4] & 0x1F;
            //printf("[ENC] NAL type=%d, len=%zu\n", nal, pkt_len);
            if (nal == 7 || nal == 8)
            {
                extract_sps_pps(packet);
                *is_sps_pps = 1;
                *is_key = 0;
            }
            else if (nal == 5)
            {
                *is_key = 1;
            }
            else if (nal == 1)
            {
                *is_key = 0;
            }
        }

        if (pkt_len > sizeof(g_out_buf))
        {
            printf("[ENC] encoded frame too large: %zu\n", pkt_len);
            goto out;
        }
        memcpy(g_out_buf, pkt_data, pkt_len);
        *out = g_out_buf;
        *out_len = pkt_len;
        ret = 0;
    }
    else //MJPEG_SW
    {
        return -1;
    }

out:
    if (packet) mpp_packet_deinit(&packet);
    if (!frame_sent && frame)
    {
        if (buf) mpp_buffer_put(buf);
        mpp_frame_deinit(&frame);
    }
    return ret;
}

void encoder_get_sps_pps(EncoderSpsPps *sps_pps)
{
    if (sps_pps && g_sps_pps.valid) 
    {
        memcpy(sps_pps, &g_sps_pps, sizeof(EncoderSpsPps));
    } 
    else if (sps_pps) 
    {
        memset(sps_pps, 0, sizeof(EncoderSpsPps));
    }
}

void encoder_destroy(void)
{
    if (g_enc_type == ENC_MJPEG)
    {
        return;
    }
    if (g_enc_type == ENC_MJPEG_SW) 
    {        
        return;
    }
    // MPP 路径
    if (g_buf_grp) 
    {
        mpp_buffer_group_put(g_buf_grp);
        g_buf_grp = NULL;
    }
    if (g_mpp_ctx)
    {
        mpp_destroy(g_mpp_ctx);
        g_mpp_ctx = NULL;
        g_mpp_mpi = NULL;
    }
    free(g_nv12_buf);
    g_nv12_buf = NULL;
    free(g_packed_buf);
    g_packed_buf = NULL;
    g_packed_size = 0;
    decode_destroy();

    printf("[ENC] encoder destroyed\n");
}