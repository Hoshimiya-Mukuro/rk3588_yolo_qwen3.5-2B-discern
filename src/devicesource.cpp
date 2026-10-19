#include "devicesource.hh"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ── H.264 Annex-B start code 工具 ── */
/* 返回 p 处 start code 的长度（4 或 3），0 表示不是 start code */
static size_t start_code_len(const uint8_t* d, size_t len, size_t p)
{
    if (p + 4 <= len && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 0 && d[p + 3] == 1) return 4;
    if (p + 3 <= len && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) return 3;
    return 0;
}

/* 从 from 开始找下一个 start code 的位置；找不到返回 len */
static size_t find_next_start_code(const uint8_t* d, size_t len, size_t from)
{
    for (size_t i = from; i + 3 <= len; i++)
    {
        if (start_code_len(d, len, i)) return i;
    }
    return len;
}

/* ========================================================================
 *  H264DeviceSource — H.264 源 (FramedSource 基类)
 * ======================================================================== */

H264DeviceSource* H264DeviceSource::createNew(UsageEnvironment& env, spsc_ring* ringbuf)
{
    return new H264DeviceSource(env, ringbuf);
}

H264DeviceSource::H264DeviceSource(UsageEnvironment& env, spsc_ring* ringbuf)
    : FramedSource(env),
      fRingbuf(ringbuf),
      fPending(NULL),
      fPendingCap(0),
      fPendingLen(0),
      fPendingPos(0)
{
    gettimeofday(&fCurrTime, NULL);
    fPacketTime = fCurrTime;
}

H264DeviceSource::~H264DeviceSource() { free(fPending); }

void H264DeviceSource::doGetNextFrameStatic(void* clientData)
{
    H264DeviceSource* source = static_cast<H264DeviceSource*>(clientData);
    source->doGetNextFrame();
}

void H264DeviceSource::doGetNextFrame()
{
    /* ── 需要时从 ring 取一整个 MPP 包（IDR 包含 SPS+PPS+IDR）── */
    if (fPendingLen == 0)
    {
        size_t frame_size = spsc_peek(fRingbuf);
        if (frame_size == 0)
        {
            envir().taskScheduler().scheduleDelayedTask(10000,
                (TaskFunc*)&H264DeviceSource::doGetNextFrameStatic, this);
            return;
        }
        /* 按需扩容，不丢大帧（1080p 的 IDR 可能几百 KB） */
        if (frame_size > fPendingCap)
        {
            uint8_t* nb = (uint8_t*)realloc(fPending, frame_size);
            if (!nb)
            {
                printf("[RTSP] realloc %zu failed, packet dropped\n", frame_size);
                spsc_skip(fRingbuf);
                envir().taskScheduler().scheduleDelayedTask(10000,
                    (TaskFunc*)&H264DeviceSource::doGetNextFrameStatic, this);
                return;
            }
            fPending = nb;
            fPendingCap = frame_size;
        }
        size_t len = fPendingCap;
        if (!spsc_read(fRingbuf, fPending, &len))
        {
            envir().taskScheduler().scheduleDelayedTask(10000,
                (TaskFunc*)&H264DeviceSource::doGetNextFrameStatic, this);
            return;
        }
        fPendingLen = len;
        fPendingPos = 0;

        /* 同一包内所有 NAL 用同一个时间戳（同属一个 access unit），整包发完才推进一帧 */
        fPacketTime = fCurrTime;
        fCurrTime.tv_usec += 33000;
        if (fCurrTime.tv_usec >= 1000000)
        {
            fCurrTime.tv_usec -= 1000000;
            fCurrTime.tv_sec += 1;
        }
    }

    /* ── 从当前包里切出下一个 NAL，并剥掉 start code ── */
    const uint8_t* nal = NULL;
    size_t nal_len = 0;
    while (fPendingPos < fPendingLen)
    {
        size_t sc = start_code_len(fPending, fPendingLen, fPendingPos);
        size_t nal_start = fPendingPos + sc;
        if (nal_start >= fPendingLen) break;

        size_t nal_end = find_next_start_code(fPending, fPendingLen, nal_start + 1);
        nal_len = nal_end - nal_start;
        nal     = fPending + nal_start;
        fPendingPos = nal_end;
        if (fPendingPos >= fPendingLen) fPendingLen = 0;   /* 本包已取完 */

        if (nal_len > 0) break;                            /* 空 NAL（连续 start code）跳过 */
    }

    if (nal == NULL || nal_len == 0)
    {
        /* 包里没有有效 NAL，丢弃等下一包 */
        fPendingLen = 0;
        envir().taskScheduler().scheduleDelayedTask(10000,
            (TaskFunc*)&H264DeviceSource::doGetNextFrameStatic, this);
        return;
    }

    if (nal_len > fMaxSize)
    {
        printf("[RTSP] H264 NAL too large: %zu > %u, packet dropped\n", nal_len, fMaxSize);
        fPendingLen = 0;
        envir().taskScheduler().scheduleDelayedTask(1000,
            (TaskFunc*)&H264DeviceSource::doGetNextFrameStatic, this);
        return;
    }

    memcpy(fTo, nal, nal_len);
    fFrameSize = (unsigned)nal_len;
    fPresentationTime = fPacketTime;

    afterGetting(this);
}


/* ========================================================================
 *  MJPEGDeviceSource — MJPEG 源 (JPEGVideoSource 基类)
 * ======================================================================== */

MJPEGDeviceSource* MJPEGDeviceSource::createNew(UsageEnvironment& env, spsc_ring* ringbuf)
{
    return new MJPEGDeviceSource(env, ringbuf);
}

MJPEGDeviceSource::MJPEGDeviceSource(UsageEnvironment& env, spsc_ring* ringbuf)
    : JPEGVideoSource(env),
      fRingbuf(ringbuf),
      fJpegType(0),
      fJpegQFactor(128),
      fJpegWidth(0),
      fJpegHeight(0),
      fQuantTablesLen(0),
      fQuantPrecision(0),
      fJpegParamsValid(false)
{
    gettimeofday(&fCurrTime, NULL);
}

MJPEGDeviceSource::~MJPEGDeviceSource() {}

void MJPEGDeviceSource::doGetNextFrameStatic(void* clientData)
{
    MJPEGDeviceSource* source = static_cast<MJPEGDeviceSource*>(clientData);
    source->doGetNextFrame();
}

void MJPEGDeviceSource::doGetNextFrame()
{
    /* 从 ring buffer 读取一帧 */
    size_t frame_size = spsc_peek(fRingbuf);
    if (frame_size == 0)
    {
        envir().taskScheduler().scheduleDelayedTask(10000,
            (TaskFunc*)&MJPEGDeviceSource::doGetNextFrameStatic, this);
        return;
    }
    if (frame_size > fMaxSize)
    {
        printf("[RTSP] MJPEG frame too large: %zu > %u, dropped\n", frame_size, fMaxSize);
        spsc_skip(fRingbuf);
        envir().taskScheduler().scheduleDelayedTask(1000,
            (TaskFunc*)&MJPEGDeviceSource::doGetNextFrameStatic, this);
        return;
    }

    size_t len = fMaxSize;
    if (!spsc_read(fRingbuf, fTo, &len))
    {
        envir().taskScheduler().scheduleDelayedTask(10000,
            (TaskFunc*)&MJPEGDeviceSource::doGetNextFrameStatic, this);
        return;
    }

    /* 校验 MJPEG 帧 */
    if (len < 2 || fTo[0] != 0xFF || fTo[1] != 0xD8)
    {
        fprintf(stderr, "[RTSP] invalid MJPEG frame, skipping\n");
        envir().taskScheduler().scheduleDelayedTask(10000,
            (TaskFunc*)&MJPEGDeviceSource::doGetNextFrameStatic, this);
        return;
    }

    /* 首帧：解析 JPEG 头 */
    if (!fJpegParamsValid)
    {
        parseJPEGHeaders((const uint8_t*)fTo, len);
    }

    fFrameSize = (unsigned)len;
    fPresentationTime = fCurrTime;
    fCurrTime.tv_usec += 33000;
    if (fCurrTime.tv_usec >= 1000000)
    {
        fCurrTime.tv_usec -= 1000000;
        fCurrTime.tv_sec += 1;
    }

    afterGetting(this);
}

/* JPEGEVideoSource 接口实现 */
u_int8_t MJPEGDeviceSource::type() { return fJpegType; }
u_int8_t MJPEGDeviceSource::qFactor() { return fJpegQFactor; }
u_int8_t MJPEGDeviceSource::width() { return fJpegWidth; }
u_int8_t MJPEGDeviceSource::height() { return fJpegHeight; }

u_int8_t const* MJPEGDeviceSource::quantizationTables(u_int8_t& precision, u_int16_t& length)
{
    if (fJpegQFactor == 0 && fQuantTablesLen > 0)
    {
        precision = fQuantPrecision;
        length = fQuantTablesLen;
        return fQuantTables;
    }
    precision = 0;
    length = 0;
    return NULL;
}

bool MJPEGDeviceSource::parseJPEGHeaders(const uint8_t* data, size_t len)
{
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;

    size_t i = 2;
    int width = 0, height = 0;
    uint8_t precision = 0;
    uint8_t quant_buf[512];
    uint16_t quant_len = 0;
    uint8_t quant_prec = 0;

    while (i + 3 < len)
    {
        if (data[i] != 0xFF) break;
        uint8_t marker = data[i + 1];
        if (marker == 0xD9 || marker == 0xDA) break;

        uint16_t seg_len = (uint16_t)((data[i + 2] << 8) | data[i + 3]);
        if (seg_len < 2 || i + 2 + seg_len > len) break;

        if (marker == 0xC0 || marker == 0xC1 || marker == 0xC2)
        {
            if (seg_len >= 7)
            {
                precision = data[i + 4];
                height    = (int)((data[i + 5] << 8) | data[i + 6]);
                width     = (int)((data[i + 7] << 8) | data[i + 8]);
            }
        }
        else if (marker == 0xDB)
        {
            int off = 2;
            while (off + 65 <= (int)seg_len && quant_len + 65 <= (int)sizeof(quant_buf))
            {
                uint8_t info = data[i + 2 + off];
                uint8_t prec = (info >> 4) & 0x0F;
                if (prec > quant_prec) quant_prec = prec;

                int tbl_size = (prec == 0) ? 64 : 128;
                if (off + 1 + tbl_size > (int)seg_len) break;
                if (quant_len + 1 + tbl_size > (int)sizeof(quant_buf)) break;

                quant_buf[quant_len++] = info;
                memcpy(quant_buf + quant_len, data + i + 2 + off + 1, tbl_size);
                quant_len += tbl_size;
                off += 1 + tbl_size;
            }
        }
        i += 2 + seg_len;
    }

    if (width == 0 || height == 0) return false;

    fJpegWidth  = (u_int8_t)(width / 8);
    fJpegHeight = (u_int8_t)(height / 8);
    if (width  % 8 != 0) fJpegWidth++;
    if (height % 8 != 0) fJpegHeight++;
    fJpegType = 0;

    if (quant_len > 0)
    {
        fJpegQFactor = 0;
        fQuantPrecision = quant_prec;
        fQuantTablesLen = quant_len;
        memcpy(fQuantTables, quant_buf, quant_len);
        printf("[RTSP] parsed JPEG: %dx%d, %d quant tables (%d bytes)\n",
               width, height, quant_len / (quant_prec ? 129 : 65), quant_len);
    }
    else
    {
        fJpegQFactor = 128;
        fQuantTablesLen = 0;
        printf("[RTSP] parsed JPEG: %dx%d, using default quant tables\n", width, height);
    }

    fJpegParamsValid = true;
    return true;
}
