#include "ringbuf.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>



extern inline void spsc_init(spsc_ring *p)
{
    p->write_idx = 0;
    p->read_idx = 0;
}

extern inline int spsc_write(spsc_ring *p, const void *data, size_t len)
{
    if (!data || len == 0 || len > SLOT_SIZE)
    {
        return 0;
    }

    uint32_t w = p->write_idx % SLOT_COUNT;
    memcpy(p->ring_data[w].buf, data, len);
    p->ring_data[w].len = len;

    __sync_synchronize();               
    p->write_idx = w + 1;
    return 1;
}

extern inline int spsc_read(spsc_ring *p, void *data, size_t *len)
{
    __sync_synchronize();

    uint32_t r = p->read_idx % SLOT_COUNT;
    uint32_t w = p->write_idx % SLOT_COUNT;
    if (r == w)
    {
        return 0;
    }

    size_t cap = *len;
    if (cap == 0)
    {
        /* 调用方没给容量，宁可返回失败也不越界写 */
        return 0;
    }

    size_t slot_len = p->ring_data[r].len;
    size_t copy_len = (slot_len > cap) ? cap : slot_len;
    if (copy_len < slot_len)
    {
        printf("[RING] frame truncated: slot=%zu cap=%zu\n", slot_len, cap);
    }

    memcpy(data, p->ring_data[r].buf, copy_len);
    *len = copy_len;

    p->read_idx = r + 1;
    return 1;
}

extern inline size_t spsc_peek(spsc_ring *p)
{
    __sync_synchronize();

    uint32_t r = p->read_idx % SLOT_COUNT;
    uint32_t w = p->write_idx % SLOT_COUNT;
    if (r == w)
    {
        return 0;
    }
    return p->ring_data[r].len;
}

extern inline void spsc_skip(spsc_ring *p)
{
    __sync_synchronize();

    uint32_t r = p->read_idx % SLOT_COUNT;
    uint32_t w = p->write_idx % SLOT_COUNT;
    if (r == w)
    {
        return;
    }
    p->read_idx = r + 1;
}







