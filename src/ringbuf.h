#ifndef __RINGBUF_H__
#define __RINGBUF_H__

#include <stddef.h>
#include <stdint.h>

#define SLOT_COUNT 3
#define SLOT_SIZE (5 * 1024 * 1024)

typedef struct 
{
    uint8_t buf[SLOT_SIZE];
    size_t len;
} spsc_slot;

typedef struct
{
    spsc_slot ring_data[SLOT_COUNT];
    volatile uint32_t write_idx;
    volatile uint32_t read_idx;
} spsc_ring;

inline void spsc_init(spsc_ring *p);
inline int spsc_write(spsc_ring *p, const void *data, size_t len);
/* *len 传入目标缓冲区容量（必须 > 0），返回实际拷贝长度。
 * 拷贝长度绝不会超过 *len，避免消费者缓冲区被写爆。 */
inline int spsc_read(spsc_ring *p, void *data, size_t *len);
/* peek：返回下一帧长度，不消费；返回 0 表示环形缓冲为空 */
inline size_t spsc_peek(spsc_ring *p);
/* skip：丢弃下一帧（不拷贝数据） */
inline void spsc_skip(spsc_ring *p);

#endif