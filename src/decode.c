#include "decode.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "turbojpeg.h"
#include "rga.h"
#include "drmrga.h"
#include "RgaApi.h"   /* c_RkRgaBlit 只在这里声明（rga.h/drmrga.h 不含它） */

/* ── 全局复用：避免每帧 tjInitDecompress / tjDestroy ── */
static tjhandle g_tj = NULL;
static uint8_t *g_u_tmp = NULL;  /* YUV420P→NV12 交织用，预分配避免每帧 malloc */
static int g_u_tmp_size = 0;

int decode_init(void)
{
    if (!g_tj)
        g_tj = tjInitDecompress();
    if (!g_tj)
    {
        fprintf(stderr, "[DECODE] tjInitDecompress failed: %s\n", tjGetErrorStr());
        return -1;
    }
    return 0;
}

void decode_destroy(void)
{
    if (g_tj)
    {
        tjDestroy(g_tj);
        g_tj = NULL;
    }
    free(g_u_tmp);
    g_u_tmp = NULL;
    g_u_tmp_size = 0;
}

int mjpeg_to_nv12(const uint8_t *mjpeg_data, size_t mjpeg_len, uint8_t *nv12_out, int width, int height)
{
    if (!g_tj)
    {
        fprintf(stderr, "[DECODE] tj handle not initialized, call decode_init() first\n");
        return -1;
    }

    /* tjDecompressToYUV2: align=1 即每行无填充 */
    int ret = tjDecompressToYUV2(g_tj, mjpeg_data, mjpeg_len, nv12_out, width, 1, height, TJFLAG_FASTDCT);
    if (ret != 0)
    {
        fprintf(stderr, "[DECODE] tjDecompressToYUV2 failed: %s\n", tjGetErrorStr2(g_tj));
        return -1;
    }

    /* ── YUV420P → NV12 交织 ──
     *
     * YUV420P 布局:  Y[0..y_size) | U[0..uv_size) | V[0..uv_size)
     * NV12 布局:     Y[0..y_size) | UV interleaved (U0,V0,U1,V1,...)
     *
     * 只拷贝 U 平面到临时缓冲（UV 区域前半 = U 平面，原地交织会覆盖未读的 U）。
     * V 平面在 UV 区域后半，正向交织时 V[i] 已被使用后才可能被覆盖，因此无需拷贝 V。 */

    int y_size = width * height;
    int uv_size = (width / 2) * (height / 2);

    /* 按需扩容临时缓冲 */
    if (g_u_tmp_size < uv_size)
    {
        uint8_t *new_buf = (uint8_t *)realloc(g_u_tmp, uv_size);
        if (!new_buf) return -1;
        g_u_tmp = new_buf;
        g_u_tmp_size = uv_size;
    }

    uint8_t *uv_dst = nv12_out + y_size;
    memcpy(g_u_tmp, uv_dst, uv_size);           /* 保存 U 平面副本 */

    for (int i = 0; i < uv_size; i++)
    {
        uv_dst[i * 2]     = g_u_tmp[i];          /* U[i] from temp */
        uv_dst[i * 2 + 1] = uv_dst[uv_size + i]; /* V[i] 原始位置，在写入到此位置前已被读取 */
    }

    return 0;
}

/* ========================================================================
 *  NV12 → RGB888（RGA 硬件加速），给 YOLO 之类的视觉模型喂数据
 * ======================================================================== */

/* 公共实现：NV12(紧凑) → RGB888。可只写目标图的一个子矩形（dst_x/dst_y/dst_w/dst_h），
 * 源和目标尺寸不同时 RGA 顺带做硬件缩放。
 *
 * 用旧版 C API（c_RkRgaBlit）：项目自带的 include/rga + lib/librga.so 就够用，
 * 不必引入 rknn_model_zoo 的 im2d 头文件。
 *
 * 输入必须是**紧凑** NV12（wstride == width、hstride == height），
 * 即 mjpeg_to_nv12() 的输出布局。
 * 注意 encode.c 里喂 MPP 的 g_nv12_buf 是带 stride 的（hor_stride/ver_stride），
 * 那个 buffer 不能直接传进来（RGA 会按紧凑布局解释，图像会斜切）。 */
static int rga_nv12_to_rgb_rect(const void *nv12, int src_w, int src_h,
                                void *rgb, int rgb_wstride, int rgb_hstride,
                                int dst_x, int dst_y, int dst_w, int dst_h)
{
    rga_info_t src;
    rga_info_t dst;
    int ret;

    memset(&src, 0, sizeof(src));
    src.fd      = -1;                 /* 用虚拟地址，不走 dma-buf fd */
    src.virAddr = (void *)nv12;
    src.mmuFlag = 1;                  /* 传虚拟地址时必须开 MMU */
    if (rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h,
                     RK_FORMAT_YCbCr_420_SP) != 0)
    {
        fprintf(stderr, "[RGA] set src rect failed\n");
        return -1;
    }

    memset(&dst, 0, sizeof(dst));
    dst.fd      = -1;
    dst.virAddr = rgb;
    dst.mmuFlag = 1;
    /* wstride/hstride 按 RGA 约定传“像素数”而非字节数：
     * RGB888 整幅宽 640 就传 640（不是 1920）。
     * 万一图像出现横向斜切/被压扁，说明这个 RGA 版本按字节解释 stride，
     * 把 rgb_wstride 换成 rgb_wstride * 3 再试。 */
    if (rga_set_rect(&dst.rect, dst_x, dst_y, dst_w, dst_h, rgb_wstride, rgb_hstride,
                     RK_FORMAT_RGB_888) != 0)
    {
        fprintf(stderr, "[RGA] set dst rect failed\n");
        return -1;
    }

    ret = c_RkRgaBlit(&src, &dst, NULL);
    if (ret < 0)
    {
        fprintf(stderr, "[RGA] nv12->rgb888 blit failed: ret=%d errno=%d(%s)\n",
                ret, errno, strerror(errno));
        return -1;
    }
    return 0;
}

int nv12_to_rgb_rga(const void *nv12_data, void *rgb_out, int width, int height)
{
    if (!nv12_data || !rgb_out || width <= 0 || height <= 0)
    {
        fprintf(stderr, "[RGA] nv12_to_rgb_rga: invalid args\n");
        return -1;
    }
    /* RGA 对 NV12 宽度有 16 对齐要求（1920 满足，非对齐宽度会直接失败） */
    if ((width & 0xF) != 0)
    {
        fprintf(stderr, "[RGA] width=%d 非 16 对齐，RGA 可能失败\n", width);
    }
    return rga_nv12_to_rgb_rect(nv12_data, width, height, rgb_out, width, height,
                                0, 0, width, height);
}

int nv12_to_rgb_rga_resize(const void *nv12_data, int src_w, int src_h,
                           void *rgb_out, int dst_w, int dst_h)
{
    if (!nv12_data || !rgb_out || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0)
    {
        fprintf(stderr, "[RGA] nv12_to_rgb_rga_resize: invalid args\n");
        return -1;
    }
    return rga_nv12_to_rgb_rect(nv12_data, src_w, src_h, rgb_out, dst_w, dst_h,
                                0, 0, dst_w, dst_h);
}

int nv12_to_rgb_rga_letterbox(const void *nv12_data, int src_w, int src_h,
                              void *rgb_out, int dst_w, int dst_h, int pad_color,
                              float *out_scale, int *out_pad_x, int *out_pad_y)
{
    if (!nv12_data || !rgb_out || src_w <= 0 || src_h <= 0 ||
        dst_w <= 0 || dst_h <= 0)
    {
        fprintf(stderr, "[RGA] nv12_to_rgb_rga_letterbox: invalid args\n");
        return -1;
    }

    /* ── 等比缩放 + 居中留边（和 rknn_model_zoo 的 convert_image_with_letterbox 同算法）── */
    float scale_w = (float)dst_w / (float)src_w;
    float scale_h = (float)dst_h / (float)src_h;
    float scale;
    int resize_w = dst_w;
    int resize_h = dst_h;
    int pad_x = 0;
    int pad_y = 0;

    if (scale_w < scale_h)
    {
        scale = scale_w;                       /* 宽度受限，上下留边 */
        resize_h = (int)(src_h * scale);
    }
    else
    {
        scale = scale_h;                       /* 高度受限，左右留边 */
        resize_w = (int)(src_w * scale);
    }

    /* 轻微对齐（RGA 对宽 4 对齐、高 2 对齐更稳），并保证留边为偶数 */
    if (resize_w % 4) resize_w -= resize_w % 4;
    if (resize_h % 2) resize_h -= resize_h % 2;

    if (scale_w < scale_h)
    {
        pad_y = (dst_h - resize_h) / 2;
        if (pad_y % 2) pad_y--;
        if (pad_y < 0) pad_y = 0;
    }
    else
    {
        pad_x = (dst_w - resize_w) / 2;
        if (pad_x % 2) pad_x--;
        if (pad_x < 0) pad_x = 0;
    }

    /* 整幅先刷成灰边色，再把缩放后的画面贴进中间 */
    memset(rgb_out, pad_color & 0xFF, (size_t)dst_w * dst_h * 3);

    if (rga_nv12_to_rgb_rect(nv12_data, src_w, src_h, rgb_out, dst_w, dst_h,
                             pad_x, pad_y, resize_w, resize_h) != 0)
    {
        return -1;
    }

    if (out_scale) *out_scale = scale;
    if (out_pad_x) *out_pad_x = pad_x;
    if (out_pad_y) *out_pad_y = pad_y;
    return 0;
}

