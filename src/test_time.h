#ifndef __TEST_TIME_H__
#define __TEST_TIME_H__

#include <sys/time.h>
#include <stdio.h>

// 全局起始时间（extern，所有 .c 文件共享同一份）
extern struct timeval g_start_time;

static inline void timer_init(void) {
    gettimeofday(&g_start_time, NULL);
}

/* 调试打点开关：默认关闭。
 * 之前是每帧 11 次 printf+fflush（30fps ≈ 330 行/秒），在串口/管道上会直接
 * 拖慢采集线程、污染延迟测量，所以默认编成空语句。
 * 需要时：编译加 -DNC2RTSP_ENABLE_LOG_TIME，或取消下面这行的注释。 */
// #define NC2RTSP_ENABLE_LOG_TIME

#ifdef NC2RTSP_ENABLE_LOG_TIME
#define LOG_TIME(msg) do { \
    struct timeval now; \
    gettimeofday(&now, NULL); \
    long long elapsed_ms = (now.tv_sec - g_start_time.tv_sec) * 1000LL + \
                           (now.tv_usec - g_start_time.tv_usec) / 1000LL; \
    printf("[%lld ms] %s\n", elapsed_ms, msg); \
    fflush(stdout); \
} while(0)
#else
#define LOG_TIME(msg) do { } while(0)
#endif

#endif /* __TEST_TIME_H__ */
