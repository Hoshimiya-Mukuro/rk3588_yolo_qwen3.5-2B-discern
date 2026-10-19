// ============================================================================
//  YOLOv8 (RKNN / RK3588 NPU) 识别模块
//
//  数据流：采集/编码线程 --yolo_publish_frame()--> 最新帧邮箱
//          --> AI 线程：NV12 --RGA letterbox--> RGB888 --NPU--> 后处理 --> 结果
//
//  设计要点：
//   * 旁路是非阻塞的：AI 线程忙时直接丢帧，采集/推流线程绝不被拖住。
//   * g_busy 在 RGA 转换完成（不再读 g_stage）后立刻清零，
//     这样采集线程投递下一帧可以和 NPU 推理重叠，提高吞吐。
//   * 所有跨线程共享量都在 g_lock 保护下访问。
// ============================================================================

#include "yolo.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "decode.h"
#include "rknn_api.h"
#include "vlm_link.h"
#include "yolo_postprocess.h"

#define YOLO_PAD_COLOR  114   /* letterbox 灰边（和 zoo 一致） */
#define YOLO_LOG_EVERY  30    /* 每 N 帧打印一次统计，避免刷屏 */

/* ── 模型与推理状态（仅 AI 线程访问） ── */
static rknn_app_context_t g_ctx;
static int          g_model_ready = 0;
static uint8_t     *g_rgb = NULL;        /* 模型输入：letterbox 后的 RGB888 */
static size_t       g_rgb_size = 0;
static letterbox_t  g_letterbox;

/* ── 线程与最新帧邮箱 ── */
static pthread_t       g_thread;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_cond = PTHREAD_COND_INITIALIZER;
static volatile int    g_run = 0;
static volatile int    g_busy = 0;       /* 1 = AI 正在读 g_stage */
static uint8_t        *g_stage = NULL;   /* 待识别的紧凑 NV12 */
static size_t          g_stage_cap = 0;
static int             g_stage_w = 0;
static int             g_stage_h = 0;
static long long       g_seq = 0;

/* ── 最近一次识别结果 ── */
static yolo_result_t g_result;
static volatile int  g_result_new = 0;

static int      g_inited = 0;
static long long g_ok_cnt = 0;
static long long g_drop_cnt = 0;
static double    g_ms_sum = 0;

static double now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec * 1000.0 + (double)tv.tv_usec / 1000.0;
}

/* 把整个文件读进内存（替代 zoo 的 file_utils::read_data_from_file） */
static int read_file(const char *path, char **out_buf, int *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    FILE *fp = fopen(path, "rb");
    if (fp == NULL)
    {
        printf("[YOLO] open model '%s' fail\n", path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0)
    {
        printf("[YOLO] model '%s' size=%ld\n", path, size);
        fclose(fp);
        return -1;
    }

    char *buf = (char *)malloc((size_t)size);
    if (buf == NULL)
    {
        fclose(fp);
        return -1;
    }
    if (fread(buf, 1, (size_t)size, fp) != (size_t)size)
    {
        printf("[YOLO] read model '%s' fail\n", path);
        free(buf);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    *out_buf = buf;
    *out_len = (int)size;
    return 0;
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%u, name=%s, n_dims=%u, dims=[%u,%u,%u,%u], n_elems=%u, size=%u, "
           "fmt=%s, type=%s, qnt_type=%s, zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1],
           attr->dims[2], attr->dims[3], attr->n_elems, attr->size,
           get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

/* 加载模型并查询输入输出（移植自 zoo rknpu2/yolov8.cc 的 init_yolov8_model） */
static int yolo_init_model(const char *model_path)
{
    int ret;
    int model_len = 0;
    char *model = NULL;
    rknn_context ctx = 0;

    if (read_file(model_path, &model, &model_len) != 0)
    {
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0)
    {
        printf("[YOLO] rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC)
    {
        printf("[YOLO] rknn_query(IN_OUT_NUM) fail! ret=%d\n", ret);
        return -1;
    }
    printf("[YOLO] model input num=%u, output num=%u\n", io_num.n_input, io_num.n_output);

    g_ctx.input_attrs = (rknn_tensor_attr *)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    g_ctx.output_attrs = (rknn_tensor_attr *)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    if (g_ctx.input_attrs == NULL || g_ctx.output_attrs == NULL)
    {
        printf("[YOLO] malloc tensor attrs fail\n");
        return -1;
    }
    memset(g_ctx.input_attrs, 0, io_num.n_input * sizeof(rknn_tensor_attr));
    memset(g_ctx.output_attrs, 0, io_num.n_output * sizeof(rknn_tensor_attr));

    printf("[YOLO] input tensors:\n");
    for (uint32_t i = 0; i < io_num.n_input; i++)
    {
        g_ctx.input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &g_ctx.input_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("[YOLO] rknn_query(INPUT_ATTR) fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&g_ctx.input_attrs[i]);
    }

    printf("[YOLO] output tensors:\n");
    for (uint32_t i = 0; i < io_num.n_output; i++)
    {
        g_ctx.output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &g_ctx.output_attrs[i], sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC)
        {
            printf("[YOLO] rknn_query(OUTPUT_ATTR) fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&g_ctx.output_attrs[i]);
    }

    g_ctx.rknn_ctx = ctx;
    g_ctx.io_num = io_num;
    g_ctx.is_quant = (g_ctx.output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                      g_ctx.output_attrs[0].type == RKNN_TENSOR_INT8);

    if (g_ctx.input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("[YOLO] model is NCHW input fmt\n");
        g_ctx.model_channel = g_ctx.input_attrs[0].dims[1];
        g_ctx.model_height  = g_ctx.input_attrs[0].dims[2];
        g_ctx.model_width   = g_ctx.input_attrs[0].dims[3];
    }
    else
    {
        printf("[YOLO] model is NHWC input fmt\n");
        g_ctx.model_height  = g_ctx.input_attrs[0].dims[1];
        g_ctx.model_width   = g_ctx.input_attrs[0].dims[2];
        g_ctx.model_channel = g_ctx.input_attrs[0].dims[3];
    }
    printf("[YOLO] model input: %dx%d ch=%d, quant=%d\n",
           g_ctx.model_width, g_ctx.model_height, g_ctx.model_channel, (int)g_ctx.is_quant);

    g_model_ready = 1;
    return 0;
}

static void yolo_release_model(void)
{
    if (g_ctx.input_attrs)
    {
        free(g_ctx.input_attrs);
        g_ctx.input_attrs = NULL;
    }
    if (g_ctx.output_attrs)
    {
        free(g_ctx.output_attrs);
        g_ctx.output_attrs = NULL;
    }
    if (g_ctx.rknn_ctx != 0)
    {
        rknn_destroy(g_ctx.rknn_ctx);
        g_ctx.rknn_ctx = 0;
    }
    g_model_ready = 0;
}

/* 输入已经躺在 g_rgb 里（letterbox 完成），这里只做 NPU 推理 + 后处理 */
static int yolo_infer(object_detect_result_list *od)
{
    rknn_input  inputs[1];
    rknn_output outputs[16];

    if (g_ctx.io_num.n_output > 16)
    {
        printf("[YOLO] too many outputs: %u\n", g_ctx.io_num.n_output);
        return -1;
    }
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    inputs[0].index = 0;
    inputs[0].type  = RKNN_TENSOR_UINT8;
    inputs[0].fmt   = RKNN_TENSOR_NHWC;
    inputs[0].size  = g_ctx.model_width * g_ctx.model_height * g_ctx.model_channel;
    inputs[0].buf   = g_rgb;

    int ret = rknn_inputs_set(g_ctx.rknn_ctx, g_ctx.io_num.n_input, inputs);
    if (ret < 0)
    {
        printf("[YOLO] rknn_inputs_set fail! ret=%d\n", ret);
        return -1;
    }

    ret = rknn_run(g_ctx.rknn_ctx, NULL);
    if (ret < 0)
    {
        printf("[YOLO] rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    for (uint32_t i = 0; i < g_ctx.io_num.n_output; i++)
    {
        outputs[i].index = i;
        outputs[i].want_float = (!g_ctx.is_quant);
    }
    ret = rknn_outputs_get(g_ctx.rknn_ctx, g_ctx.io_num.n_output, outputs, NULL);
    if (ret < 0)
    {
        printf("[YOLO] rknn_outputs_get fail! ret=%d\n", ret);
        return -1;
    }

    ret = post_process(&g_ctx, outputs, &g_letterbox, BOX_THRESH, NMS_THRESH, od);
    rknn_outputs_release(g_ctx.rknn_ctx, g_ctx.io_num.n_output, outputs);
    return ret;
}

static void print_result(const object_detect_result_list *od, long long seq, double cost_ms)
{
    if (od->count <= 0)
    {
        return;
    }
    printf("[YOLO] #%lld %d obj (%.1fms):", seq, od->count, cost_ms);
    for (int i = 0; i < od->count; i++)
    {
        const object_detect_result *r = &od->results[i];
        printf(" %s(%.2f)@(%d,%d,%d,%d)", coco_cls_to_name(r->cls_id), r->prop,
               r->box.left, r->box.top, r->box.right, r->box.bottom);
    }
    printf("\n");
    fflush(stdout);
}

static void *yolo_thread(void *arg)
{
    (void)arg;

    while (g_run)
    {
        pthread_mutex_lock(&g_lock);
        while (g_run && !g_busy)
        {
            pthread_cond_wait(&g_cond, &g_lock);
        }
        if (!g_run)
        {
            pthread_mutex_unlock(&g_lock);
            break;
        }
        int w = g_stage_w;
        int h = g_stage_h;
        long long seq = g_seq;
        pthread_mutex_unlock(&g_lock);

        double t0 = now_ms();

        /* ── 0) VLM 旁路：时间闸门通过才做全分辨率转换 ──
         * 必须在 g_stage 独占窗口内完成，否则会被采集线程覆盖。
         * 这里只按时间节流；真正送不送要等推理出类别后再定。 */
        uint8_t *vlm_rgb = vlm_link_acquire(w, h);
        if (vlm_rgb != NULL)
        {
            if (nv12_to_rgb_rga(g_stage, vlm_rgb, w, h) != 0)
            {
                vlm_rgb = NULL;
            }
        }

        /* ── 1) NV12 → RGB888 letterbox（RGA 硬件缩放，等比 + 灰边）── */
        int ret = nv12_to_rgb_rga_letterbox(g_stage, w, h, g_rgb,
                                            g_ctx.model_width, g_ctx.model_height,
                                            YOLO_PAD_COLOR,
                                            &g_letterbox.scale,
                                            &g_letterbox.x_pad,
                                            &g_letterbox.y_pad);

        /* g_stage 已经不再需要，立刻放开让采集线程投递下一帧（与 NPU 推理重叠） */
        pthread_mutex_lock(&g_lock);
        g_busy = 0;
        pthread_mutex_unlock(&g_lock);

        if (ret != 0)
        {
            printf("[YOLO] letterbox failed (src %dx%d)\n", w, h);
            continue;
        }

        /* ── 2) NPU 推理 + 后处理 ── */
        object_detect_result_list od;
        ret = yolo_infer(&od);
        if (ret != 0)
        {
            printf("[YOLO] inference failed\n");
            continue;
        }

        double cost_ms = now_ms() - t0;

        /* ── 3) 存结果（供业务线程消费）── */
        pthread_mutex_lock(&g_lock);
        g_result.count = od.count;
        for (int i = 0; i < od.count; i++)
        {
            g_result.objects[i].cls_id = od.results[i].cls_id;
            g_result.objects[i].prop   = od.results[i].prop;
            g_result.objects[i].left   = od.results[i].box.left;
            g_result.objects[i].top    = od.results[i].box.top;
            g_result.objects[i].right  = od.results[i].box.right;
            g_result.objects[i].bottom = od.results[i].box.bottom;
        }
        g_result.frame_w = w;
        g_result.frame_h = h;
        g_result.seq = seq;
        g_result_new = 1;
        pthread_mutex_unlock(&g_lock);

        /* ── 4) VLM 联动：命中触发类别才落盘 + UDP 通知 ── */
        if (vlm_rgb != NULL)
        {
            vlm_link_commit(&g_result, w, h, seq);
        }

        /* ── 5) 打印 + 统计 ── */
        g_ok_cnt++;
        g_ms_sum += cost_ms;
        print_result(&od, seq, cost_ms);

        if (g_ok_cnt % YOLO_LOG_EVERY == 0)
        {
            printf("[YOLO] stat: infer=%lld drop=%lld avg=%.1fms (%.1ffps)\n",
                   g_ok_cnt, g_drop_cnt, g_ms_sum / (double)g_ok_cnt,
                   1000.0 / (g_ms_sum / (double)g_ok_cnt));
            fflush(stdout);
        }
    }

    return NULL;
}

/* ============================ 对外接口 ============================ */

int yolo_start(const char *model_path, const char *label_path)
{
    if (g_inited)
    {
        return 0;
    }
    if (model_path == NULL)
    {
        printf("[YOLO] model_path is NULL\n");
        return -1;
    }

    memset(&g_ctx, 0, sizeof(g_ctx));
    memset(&g_result, 0, sizeof(g_result));

    if (yolo_init_model(model_path) != 0)
    {
        yolo_release_model();
        return -1;
    }

    init_post_process(label_path);

    g_rgb_size = (size_t)g_ctx.model_width * g_ctx.model_height * 3;
    g_rgb = (uint8_t *)malloc(g_rgb_size);
    if (g_rgb == NULL)
    {
        printf("[YOLO] malloc rgb buffer (%zu) fail\n", g_rgb_size);
        yolo_release_model();
        deinit_post_process();
        return -1;
    }

    g_run = 1;
    if (pthread_create(&g_thread, NULL, yolo_thread, NULL) != 0)
    {
        printf("[YOLO] create thread fail\n");
        g_run = 0;
        free(g_rgb);
        g_rgb = NULL;
        yolo_release_model();
        deinit_post_process();
        return -1;
    }

    g_inited = 1;
    printf("[YOLO] started: model=%s, input=%dx%d\n",
           model_path, g_ctx.model_width, g_ctx.model_height);
    return 0;
}

void yolo_stop(void)
{
    if (!g_inited)
    {
        return;
    }

    pthread_mutex_lock(&g_lock);
    g_run = 0;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);

    pthread_join(g_thread, NULL);

    yolo_release_model();
    deinit_post_process();

    free(g_rgb);
    g_rgb = NULL;
    g_rgb_size = 0;
    free(g_stage);
    g_stage = NULL;
    g_stage_cap = 0;

    g_inited = 0;
    printf("[YOLO] stopped (infer=%lld, drop=%lld)\n", g_ok_cnt, g_drop_cnt);
}

int yolo_publish_frame(const void *nv12, int width, int height)
{
    if (!g_inited || nv12 == NULL || width <= 0 || height <= 0)
    {
        return 0;
    }

    size_t need = (size_t)width * (size_t)height * 3 / 2;

    pthread_mutex_lock(&g_lock);
    if (g_busy)
    {
        /* AI 还没处理完上一帧：直接丢帧，保证采集线程不被拖住 */
        g_drop_cnt++;
        pthread_mutex_unlock(&g_lock);
        return 0;
    }

    if (need > g_stage_cap)
    {
        uint8_t *nb = (uint8_t *)realloc(g_stage, need);
        if (nb == NULL)
        {
            pthread_mutex_unlock(&g_lock);
            return 0;
        }
        g_stage = nb;
        g_stage_cap = need;
    }

    memcpy(g_stage, nv12, need);
    g_stage_w = width;
    g_stage_h = height;
    g_seq++;
    g_busy = 1;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_lock);
    return 1;
}

int yolo_get_latest_result(yolo_result_t *out)
{
    if (out == NULL)
    {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    if (!g_result_new)
    {
        pthread_mutex_unlock(&g_lock);
        return 0;
    }
    *out = g_result;
    g_result_new = 0;
    pthread_mutex_unlock(&g_lock);
    return 1;
}

const char *yolo_cls_to_name(int cls_id)
{
    return coco_cls_to_name(cls_id);
}

int yolo_snapshot(yolo_result_t *out)
{
    if (out == NULL)
    {
        return 0;
    }

    pthread_mutex_lock(&g_lock);
    *out = g_result;
    int has = (g_result.count > 0);
    pthread_mutex_unlock(&g_lock);
    return has;
}
