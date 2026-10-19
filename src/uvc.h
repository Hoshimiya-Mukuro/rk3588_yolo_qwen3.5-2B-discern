#ifndef __UVC_H__
#define __UVC_H__

#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct
{
    void *data;
    size_t len;
    int64_t pts_us;
    int buf_index;
} UvcFrame;

/* 协商结果：uvc_init 成功后 g_width/g_height 是驱动实际生效的分辨率，
 * 可能与请求值不同（驱动会降级），调用方必须用这两个值去初始化编码器。 */
extern int g_pixfmt;
extern int g_width;
extern int g_height;

bool uvc_init(const char *dev, int width, int height, int fps);
int uvc_read_frame(UvcFrame *frame);
int uvc_release_frame(UvcFrame *frame);
bool uvc_close();

#ifdef __cplusplus
}
#endif

#endif