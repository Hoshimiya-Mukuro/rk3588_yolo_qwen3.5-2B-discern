#ifndef __VLM_LINK_H__
#define __VLM_LINK_H__

/* ============================================================================
 *  VLM 联动通道（旁路）：裁剪出人像 → 落盘 PPM → UDP 通知多模态程序
 *
 *  用法（yolo.cc 的 AI 线程内调用）：
 *      1) 独占窗口内、时间闸门之后：
 *             uint8_t *rgb = vlm_link_acquire(w, h);
 *             if (rgb != NULL) nv12_to_rgb_rga(g_stage, w, h, rgb, w, h);
 *      2) 推理出结果后：
 *             vlm_link_commit(&od, w, h, seq);   // 命中触发类别才裁剪+落盘+发包
 *
 *  设计要点见《YOLO-VLM联动设计.md》§3/§4/§11：
 *   * 触发带**置信度门槛**，滤掉弱误检（否则 0.26 的假人也会唤醒 VLM）
 *   * 落盘按 seq 命名（一帧一文件），报文里的路径唯一对应那一帧，
 *     彻底消除"VLM 读到的不是被通知的那帧"的竞态；只保留最近 N 张
 *   * 落盘内容是**以人像为中心的正方形裁剪**（原分辨率不缩放），
 *     人在图里占比大幅提高，VLM 侧 expand2square 变成 no-op
 * ==========================================================================*/

#include <stdint.h>
#include "yolo.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化：UDP 目标、PPM 路径模板（如 /dev/shm/vlm_frame.ppm）、送图间隔(ms)、
 * 触发置信度门槛、触发类别表。
 * trigger_cls 为 NULL 或 cnt<=0 时默认只认 person(cls 0) */
int vlm_link_init(const char *ip, int port, const char *ppm_path,
                  int interval_ms, float trigger_conf,
                  const int *trigger_cls, int trigger_cls_cnt);

int vlm_link_enabled(void);

/* 时间闸门 + 提供全分辨率 RGB 缓冲（w*h*3，内部持有）。
 * 未启用或距上次未满 interval_ms 时返回 NULL。 */
uint8_t *vlm_link_acquire(int width, int height);

/* 裁剪 + 落盘 + 发 UDP。命中触发类别并成功发送返回 1。 */
int vlm_link_commit(const yolo_result_t *res, int width, int height, long long seq);

void vlm_link_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* __VLM_LINK_H__ */
