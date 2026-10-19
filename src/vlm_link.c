#include "vlm_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define VLM_LINK_MAX_CLS   8
#define VLM_LINK_MSG_MAX   768
#define VLM_LINK_PATH_MAX  300
#define VLM_LINK_KEEP      3      /* 保留最近几张裁剪图 */
#define VLM_LINK_CROP_MUL  2      /* 裁剪边长 = 目标长边 * 2（留一点环境上下文） */
#define VLM_LINK_CROP_MIN  96     /* 裁剪边长下限，避免把小框放大成一团糊 */
#define VLM_LINK_PROP_MIN  0.25f  /* dets 字符串里保留的最低置信度 */

static char g_ip[32];
static char g_path[VLM_LINK_PATH_MAX];   /* 模板：/dev/shm/vlm_frame.ppm */
static int  g_port = 0;
static int  g_interval_ms = 2000;
static float g_trigger_conf = 0.60f;
static int  g_trigger_cls[VLM_LINK_MAX_CLS];
static int  g_trigger_cnt = 0;

static int g_sock = -1;
static struct sockaddr_in g_dst;

static uint8_t *g_rgb = NULL;        /* 全分辨率 RGB（acquire 返回给调用方填充） */
static size_t   g_rgb_cap = 0;
static uint8_t *g_crop = NULL;       /* 裁剪结果 */
static size_t   g_crop_cap = 0;
static int      g_crop_w = 0;
static int      g_crop_h = 0;
static int      g_crop_x = 0;        /* 裁剪区在原图中的左上角 */
static int      g_crop_y = 0;

static int      g_enabled = 0;
static long long g_last_attempt_ms = 0;
static long long g_sent = 0;

/* 历史文件名环：只保留最近 VLM_LINK_KEEP 张，旧的删掉 */
static char g_hist[VLM_LINK_KEEP][VLM_LINK_PATH_MAX];
static int  g_hist_cnt = 0;
static int  g_hist_next = 0;

static long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + (long long)tv.tv_usec / 1000;
}

int vlm_link_init(const char *ip, int port, const char *ppm_path,
                  int interval_ms, float trigger_conf,
                  const int *trigger_cls, int trigger_cls_cnt)
{
    if (ip == NULL || ppm_path == NULL || port <= 0)
    {
        fprintf(stderr, "[VLM] init: bad args\n");
        return -1;
    }

    snprintf(g_ip, sizeof(g_ip), "%s", ip);
    snprintf(g_path, sizeof(g_path), "%s", ppm_path);
    g_port = port;
    g_interval_ms = (interval_ms > 0) ? interval_ms : 2000;
    g_trigger_conf = (trigger_conf > 0.0f) ? trigger_conf : 0.60f;

    g_trigger_cnt = 0;
    if (trigger_cls != NULL)
    {
        for (int i = 0; i < trigger_cls_cnt && i < VLM_LINK_MAX_CLS; i++)
        {
            g_trigger_cls[g_trigger_cnt++] = trigger_cls[i];
        }
    }
    if (g_trigger_cnt == 0)
    {
        g_trigger_cls[g_trigger_cnt++] = 0;
    }

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0)
    {
        fprintf(stderr, "[VLM] socket failed: %s\n", strerror(errno));
        return -1;
    }

    memset(&g_dst, 0, sizeof(g_dst));
    g_dst.sin_family = AF_INET;
    g_dst.sin_port = htons((uint16_t)g_port);
    g_dst.sin_addr.s_addr = inet_addr(g_ip);

    g_last_attempt_ms = 0;
    g_enabled = 1;

    printf("[VLM] link up: udp %s:%d, ppm %s, interval %dms, "
           "trigger cls=%d conf>=%.2f, keep %d\n",
           g_ip, g_port, g_path, g_interval_ms,
           g_trigger_cls[0], g_trigger_conf, VLM_LINK_KEEP);
    return 0;
}

int vlm_link_enabled(void)
{
    return g_enabled;
}

uint8_t *vlm_link_acquire(int width, int height)
{
    if (!g_enabled || width <= 0 || height <= 0)
    {
        return NULL;
    }

    long long t = now_ms();
    if (g_last_attempt_ms != 0 && (t - g_last_attempt_ms) < g_interval_ms)
    {
        return NULL;
    }
    g_last_attempt_ms = t;

    size_t need = (size_t)width * (size_t)height * 3;
    if (need > g_rgb_cap)
    {
        uint8_t *nb = (uint8_t *)realloc(g_rgb, need);
        if (nb == NULL)
        {
            fprintf(stderr, "[VLM] realloc %zu failed\n", need);
            return NULL;
        }
        g_rgb = nb;
        g_rgb_cap = need;
    }
    return g_rgb;
}

/* 选触发目标：过置信度门槛、类别命中，取面积最大的那个 */
static const yolo_object_t *pick_target(const yolo_result_t *res)
{
    const yolo_object_t *best = NULL;
    long long best_area = 0;

    for (int i = 0; i < res->count; i++)
    {
        if (res->objects[i].prop < g_trigger_conf)
        {
            continue;
        }

        int hit = 0;
        for (int k = 0; k < g_trigger_cnt; k++)
        {
            if (res->objects[i].cls_id == g_trigger_cls[k])
            {
                hit = 1;
                break;
            }
        }
        if (!hit)
        {
            continue;
        }

        long long w = res->objects[i].right - res->objects[i].left;
        long long h = res->objects[i].bottom - res->objects[i].top;
        if (w <= 0 || h <= 0)
        {
            continue;
        }
        if (w * h > best_area)
        {
            best_area = w * h;
            best = &res->objects[i];
        }
    }
    return best;
}

/* 以目标为中心裁出正方形（原分辨率，不缩放）。坐标/边长都圆整到偶数。 */
static int crop_target(const yolo_object_t *obj, int width, int height)
{
    int bw = obj->right - obj->left;
    int bh = obj->bottom - obj->top;
    int long_side = (bw > bh) ? bw : bh;

    int side = long_side * VLM_LINK_CROP_MUL;
    if (side < VLM_LINK_CROP_MIN)
    {
        side = VLM_LINK_CROP_MIN;
    }
    if (side > width)
    {
        side = width;
    }
    if (side > height)
    {
        side = height;
    }
    side &= ~1;
    if (side < 2)
    {
        return -1;
    }

    int cx = (obj->left + obj->right) / 2 - side / 2;
    int cy = (obj->top + obj->bottom) / 2 - side / 2;
    if (cx < 0)
    {
        cx = 0;
    }
    if (cy < 0)
    {
        cy = 0;
    }
    if (cx + side > width)
    {
        cx = width - side;
    }
    if (cy + side > height)
    {
        cy = height - side;
    }

    size_t need = (size_t)side * (size_t)side * 3;
    if (need > g_crop_cap)
    {
        uint8_t *nb = (uint8_t *)realloc(g_crop, need);
        if (nb == NULL)
        {
            fprintf(stderr, "[VLM] realloc crop %zu failed\n", need);
            return -1;
        }
        g_crop = nb;
        g_crop_cap = need;
    }

    for (int y = 0; y < side; y++)
    {
        memcpy(g_crop + (size_t)y * side * 3,
               g_rgb + ((size_t)(cy + y) * (size_t)width + (size_t)cx) * 3,
               (size_t)side * 3);
    }
    g_crop_w = side;
    g_crop_h = side;
    g_crop_x = cx;
    g_crop_y = cy;
    return 0;
}

/* /dev/shm/vlm_frame.ppm + 603 -> /dev/shm/vlm_frame_603.ppm */
static int build_frame_path(char *out, size_t size, long long seq)
{
    const char *dot = strrchr(g_path, '.');

    if (dot == NULL || dot == g_path)
    {
        return snprintf(out, size, "%s_%lld", g_path, seq);
    }
    return snprintf(out, size, "%.*s_%lld%s",
                    (int)(dot - g_path), g_path, seq, dot);
}

/* 记入历史环，超出的旧文件删掉 */
static void hist_push(const char *path)
{
    if (g_hist_cnt >= VLM_LINK_KEEP && g_hist[g_hist_next][0] != '\0')
    {
        unlink(g_hist[g_hist_next]);
    }
    snprintf(g_hist[g_hist_next], VLM_LINK_PATH_MAX, "%s", path);
    g_hist_next = (g_hist_next + 1) % VLM_LINK_KEEP;
    if (g_hist_cnt < VLM_LINK_KEEP)
    {
        g_hist_cnt++;
    }
}

/* PPM(P6) 写 tmp 后 rename：读者永远看不到半张图 */
static int write_ppm(const char *path, const uint8_t *rgb, int width, int height)
{
    char tmp[VLM_LINK_PATH_MAX + 8];
    size_t bytes = (size_t)width * (size_t)height * 3;
    int ok = 1;

    snprintf(tmp, sizeof(tmp), "%s.tmp", path);

    FILE *fp = fopen(tmp, "wb");
    if (fp == NULL)
    {
        fprintf(stderr, "[VLM] open %s failed: %s\n", tmp, strerror(errno));
        return -1;
    }
    if (fprintf(fp, "P6\n%d %d\n255\n", width, height) < 0)
    {
        ok = 0;
    }
    if (ok && fwrite(rgb, 1, bytes, fp) != bytes)
    {
        ok = 0;
    }
    if (fclose(fp) != 0)
    {
        ok = 0;
    }
    if (!ok)
    {
        fprintf(stderr, "[VLM] write %s failed\n", tmp);
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, path) != 0)
    {
        fprintf(stderr, "[VLM] rename to %s failed: %s\n", path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* "VLM1|<seq>|<path>|<W>x<H>|<crop_x>,<crop_y>|<dets>"
 * W/H 是裁剪图尺寸；crop_x/y 是裁剪区在原图中的左上角，
 * dets 坐标仍是**原图坐标系**，用 crop_x/y 相减即可换算到裁剪图。
 * dets 为 cls:conf:x1,y1,x2,y2，分号分隔。 */
static int build_msg(char *buf, size_t size, const char *path,
                     const yolo_result_t *res, long long seq)
{
    int n = snprintf(buf, size, "VLM1|%lld|%s|%dx%d|%d,%d|",
                     seq, path, g_crop_w, g_crop_h, g_crop_x, g_crop_y);
    if (n < 0 || (size_t)n >= size)
    {
        return -1;
    }

    int first = 1;
    for (int i = 0; i < res->count; i++)
    {
        if (res->objects[i].prop < VLM_LINK_PROP_MIN)
        {
            continue;
        }
        int m = snprintf(buf + n, size - (size_t)n, "%s%s:%.2f:%d,%d,%d,%d",
                         first ? "" : ";",
                         yolo_cls_to_name(res->objects[i].cls_id),
                         res->objects[i].prop,
                         res->objects[i].left, res->objects[i].top,
                         res->objects[i].right, res->objects[i].bottom);
        if (m < 0 || (size_t)(n + m) >= size)
        {
            break;   /* 放不下就截断，协议允许 dets 缺省 */
        }
        n += m;
        first = 0;
    }
    return n;
}

int vlm_link_commit(const yolo_result_t *res, int width, int height, long long seq)
{
    char path[VLM_LINK_PATH_MAX];
    char msg[VLM_LINK_MSG_MAX];
    int len;

    if (!g_enabled || res == NULL || g_rgb == NULL || res->count <= 0)
    {
        return 0;
    }

    const yolo_object_t *target = pick_target(res);
    if (target == NULL)
    {
        return 0;   /* 没过门槛 / 类别不命中 */
    }

    if (crop_target(target, width, height) != 0)
    {
        return 0;
    }

    if (build_frame_path(path, sizeof(path), seq) < 0)
    {
        return 0;
    }
    if (write_ppm(path, g_crop, g_crop_w, g_crop_h) != 0)
    {
        return 0;
    }
    hist_push(path);

    len = build_msg(msg, sizeof(msg), path, res, seq);
    if (len <= 0)
    {
        return 0;
    }

    if (sendto(g_sock, msg, (size_t)len, 0,
               (struct sockaddr *)&g_dst, sizeof(g_dst)) < 0)
    {
        fprintf(stderr, "[VLM] sendto failed: %s\n", strerror(errno));
        return 0;
    }

    g_sent++;
    printf("[VLM] #%lld sent (crop %dx%d, %.2f): %s\n",
           seq, g_crop_w, g_crop_h, target->prop, msg);
    fflush(stdout);
    return 1;
}

void vlm_link_deinit(void)
{
    for (int i = 0; i < VLM_LINK_KEEP; i++)
    {
        if (g_hist[i][0] != '\0')
        {
            unlink(g_hist[i]);
            g_hist[i][0] = '\0';
        }
    }
    g_hist_cnt = 0;
    g_hist_next = 0;

    if (g_sock >= 0)
    {
        close(g_sock);
        g_sock = -1;
    }
    free(g_rgb);
    g_rgb = NULL;
    g_rgb_cap = 0;
    free(g_crop);
    g_crop = NULL;
    g_crop_cap = 0;
    g_enabled = 0;
    printf("[VLM] link down (sent=%lld)\n", g_sent);
}
