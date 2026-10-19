#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <linux/videodev2.h>

#include "test_time.h"
#include "uvc.h"
#include "encode.h"
#include "vlm_link.h"
#include "yolo.h"
#include "rtsp_s.hh"

/* g_start_time 定义（在 test_time.h 中声明为 extern，供所有 .c/.cpp 文件共享） */
struct timeval g_start_time;

volatile int g_running = 1;
static spsc_ring g_ring;

/* 期望的分辨率（宽 x 高）。驱动不支持时会协商降级，
 * 实际生效值以 uvc_init 回读的 g_width/g_height 为准。 */
#define REQ_WIDTH   1920
#define REQ_HEIGHT  1080

/* YOLOv8 模型与类别名表（相对启动目录） */
#define YOLO_MODEL_PATH  "/home/test3/model/yolov8n.rknn"
#define YOLO_LABEL_PATH  "/home/test3/model/coco_80_labels_list.txt"

/* VLM 联动通道（旁路：裁剪人像 + 落盘 + UDP 通知） */
#define VLM_UDP_IP       "127.0.0.1"
#define VLM_UDP_PORT     9600
#define VLM_PPM_PATH     "/dev/shm/vlm_frame.ppm"
#define VLM_INTERVAL_MS  2000
#define VLM_TRIGGER_CONF 0.60f   /* 低于此置信度的人不唤醒 VLM，滤掉弱误检 */

void *capture_thread(void *arg)
{
    while (g_running)
    {
        UvcFrame v_data;
        if(uvc_read_frame(&v_data)<1)
        {
            usleep(10000);
            continue;
        }
         LOG_TIME("uvc_read完成");

        void *enc = NULL;
        size_t enc_len = 0;
        int is_key = 0, is_sps_pps = 0;
        uint64_t pts = 0;
        encoder_encode(v_data.data, v_data.len, &enc, &enc_len,&is_key, &is_sps_pps, v_data.pts_us);
         LOG_TIME("encode_完成");
        if (enc_len > 0)
        {
            unsigned char *p = (unsigned char *)enc;
            //printf("[ENC] output first 4 bytes: %02x %02x %02x %02x, len=%zu\n", p[0], p[1], p[2], p[3], enc_len);
            (void)p;

            int sign = spsc_write(&g_ring, enc, enc_len);
             LOG_TIME("spsc_write完成");
            if(!sign)
            {
                printf("ringbuf 满\n");
            }
        }
        uvc_release_frame(&v_data);
    }
    return NULL;
}

int main()
{
    timer_init();
    if (!uvc_init("/dev/video0", REQ_WIDTH, REQ_HEIGHT, 30))
    {
        printf("[MAIN] uvc_init failed\n");
        return -1;
    }
    LOG_TIME("uvc初始化完成");
    printf("[MAIN] 实际采集参数: %dx%d, pixfmt=0x%x\n", g_width, g_height, (unsigned)g_pixfmt);

    int enc_type;
    int bitrate = 4000000;
    int gop = 5;

    if (g_pixfmt == V4L2_PIX_FMT_MJPEG)
    {
        enc_type = ENC_MPP;
        printf("[MAIN] camera=MJPEG → decode to NV12 → MPP H.264\n");
    }
    else if (g_pixfmt == V4L2_PIX_FMT_YUYV)
    {
        enc_type = ENC_MPP;
        printf("[MAIN] camera=YUYV → MPP H.264 (通路未实现，不应走到这里)\n");
    }
    else
    {
        printf("[MAIN] unknown pixel format 0x%x\n", g_pixfmt);
        return -1;
    }

    /* 必须用协商后的实际分辨率，否则编码器配置与码流不符 → 花屏 */
    if (encoder_init(enc_type, g_width, g_height, 30, bitrate, gop) != 0)
    {
        printf("[MAIN] encoder_init failed\n");
        return -1;
    }
     LOG_TIME("encode初始化完成");

    /* ── VLM 联动通道（旁路）：裁剪人像 + 落盘 + UDP 通知，2s 抽样、person 触发 ── */
    {
        static const int trigger_cls[] = { 0 };   /* COCO person */
        if (vlm_link_init(VLM_UDP_IP, VLM_UDP_PORT, VLM_PPM_PATH, VLM_INTERVAL_MS,
                          VLM_TRIGGER_CONF, trigger_cls, 1) != 0)
        {
            printf("[MAIN] vlm_link_init failed，继续推流但没有 VLM 联动\n");
        }
    }

    /* ── 启动 YOLOv8 识别（旁路）──
     * 失败只打日志、不影响推流：没有 AI 结果但视频照常。 */
    if (yolo_start(YOLO_MODEL_PATH, YOLO_LABEL_PATH) != 0)
    {
        printf("[MAIN] yolo_start failed (%s)，继续推流但没有 AI 结果\n", YOLO_MODEL_PATH);
    }
     LOG_TIME("yolo初始化完成");

    spsc_init(&g_ring);
     LOG_TIME("spsc初始化完成");

    pthread_t tid;
    pthread_create(&tid, NULL, capture_thread, NULL);

    rtsp_server_start(8554, "live", &g_ring, enc_type);

    pthread_join(tid, NULL);
    yolo_stop();
    vlm_link_deinit();
    encoder_destroy();
    return 0;
}
