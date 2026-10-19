#ifndef __ENCODE_H__
#define __ENCODE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


#ifdef __cplusplus
extern "C" {
#endif

// ======== 编码器类型 ========
typedef enum {
    ENC_MPP        = 0,  // MPP H.264 硬编码 (RK3588/RK3568)
    ENC_MJPEG      = 1,  // MJPEG 透传 (摄像头输出直接走)
    ENC_MJPEG_SW   = 2,  // YUYV → libjpeg 软件压缩 (兜底)
} EncoderType;

// ======== SPS/PPS 缓存（给 Live555 用） ========
typedef struct {
    uint8_t  sps[128];
    uint32_t sps_len;
    uint8_t  pps[64];
    uint32_t pps_len;
    bool     valid;
} EncoderSpsPps;

//初始化编码器
int encoder_init(int type, int width, int height, int fps, int bitrate, int gop);

//编码一帧
int encoder_encode(const void *in, size_t in_len, void **out, size_t *out_len, int *is_key, int *is_sps_pps, uint64_t pts);

//获取缓存的 SPS/PPS
void encoder_get_sps_pps(EncoderSpsPps *sps_pps);

//销毁编码器，释放所有资源
void encoder_destroy(void);

#ifdef __cplusplus
}
#endif

#endif
