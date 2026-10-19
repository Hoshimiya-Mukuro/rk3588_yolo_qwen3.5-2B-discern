#ifndef __DECODE_H__
#define __DECODE_H__

#include <stddef.h>
#include <stdint.h>

/* decode.c 是 C，yolo.cc 是 C++：必须统一 C 链接，
 * 否则 C++ 侧按 C++ 修饰符找符号 → 链接阶段 undefined reference。 */
#ifdef __cplusplus
extern "C" {
#endif

int decode_init(void);
void decode_destroy(void);
int mjpeg_to_nv12(const uint8_t *mjpeg_data, size_t mjpeg_len, uint8_t *nv12_out, int width, int height);

/* ── NV12 → RGB888（RGA 硬件加速），供视觉模型（YOLO 等）使用 ──
 * nv12_data 必须是**紧凑** NV12（stride == width），即 mjpeg_to_nv12() 的输出；
 * rgb_out 由调用方分配，大小 width*height*3。
 * 返回 0 成功，-1 失败。 */
int nv12_to_rgb_rga(const void *nv12_data, void *rgb_out, int width, int height);

/* 同上，但顺带用 RGA 硬件缩放到 dst_w*dst_h（YOLO 常要 640x640）。
 * rgb_out 大小 dst_w*dst_h*3。 */
int nv12_to_rgb_rga_resize(const void *nv12_data, int src_w, int src_h,
                           void *rgb_out, int dst_w, int dst_h);

/* 等比缩放 + 居中留边（letterbox），YOLO 预处理的正确做法：
 * 直接拉伸会改变长宽比、掉精度。rgb_out 大小 dst_w*dst_h*3，先整幅刷 pad_color。
 * 返回 scale 与留边像素，用于把检测框坐标反算回原图。 */
int nv12_to_rgb_rga_letterbox(const void *nv12_data, int src_w, int src_h,
                              void *rgb_out, int dst_w, int dst_h, int pad_color,
                              float *out_scale, int *out_pad_x, int *out_pad_y);
#ifdef __cplusplus
}
#endif

#endif  /* __DECODE_H__ */
