#ifndef __DEVICESOURCE_HH__
#define __DEVICESOURCE_HH__

#include "FramedSource.hh"
#include "JPEGVideoSource.hh"
#include "compat.h"

/* ==================== H.264 设备源 (继承 FramedSource) ==================== */
class H264DeviceSource : public FramedSource
{
public:
    static H264DeviceSource* createNew(UsageEnvironment& env, spsc_ring* ringbuf);
    void doGetNextFrame();
    static void doGetNextFrameStatic(void* clientData);

protected:
    H264DeviceSource(UsageEnvironment& env, spsc_ring* ringbuf);
    virtual ~H264DeviceSource();

private:
    spsc_ring* fRingbuf;
    struct timeval fCurrTime;

    /* 当前从 ring 取到的整个 MPP 包（IDR 包通常是 SPS+PPS+IDR 三个 NAL）及拆分进度。
     * H264VideoStreamDiscreteFramer 要求每次只喂一个完整 NAL，而且**绝不能带 start code**
     * （带 start code 会让 RTP 载荷第一个字节变成 0x00，解码器解析不出 NAL type → 花屏），
     * 所以这里把整包缓存下来，剥掉 start code 后逐个 NAL 发出。 */
    uint8_t* fPending;           // 包缓存（按需扩容）
    size_t   fPendingCap;        // 缓存容量
    size_t   fPendingLen;        // 包内有效字节数，0 表示需要取新包
    size_t   fPendingPos;        // 下一个待发出 NAL 的 start code 位置
    struct timeval fPacketTime;  // 同一包内所有 NAL 共用（它们属于同一个 access unit）
};

/* ==================== MJPEG 设备源 (继承 JPEGVideoSource) ==================== */
class MJPEGDeviceSource : public JPEGVideoSource
{
public:
    static MJPEGDeviceSource* createNew(UsageEnvironment& env, spsc_ring* ringbuf);
    void doGetNextFrame();
    static void doGetNextFrameStatic(void* clientData);

protected:
    MJPEGDeviceSource(UsageEnvironment& env, spsc_ring* ringbuf);
    virtual ~MJPEGDeviceSource();

    // JPEGVideoSource pure virtual methods
    virtual u_int8_t type();
    virtual u_int8_t qFactor();
    virtual u_int8_t width();
    virtual u_int8_t height();
    virtual u_int8_t const* quantizationTables(u_int8_t& precision, u_int16_t& length);

private:
    bool parseJPEGHeaders(const uint8_t* data, size_t len);

    spsc_ring* fRingbuf;
    struct timeval fCurrTime;

    // Cached JPEG parameters (parsed from first frame's headers)
    u_int8_t  fJpegType;
    u_int8_t  fJpegQFactor;
    u_int8_t  fJpegWidth;       // 实际像素 / 8
    u_int8_t  fJpegHeight;      // 实际像素 / 8
    u_int8_t  fQuantTables[512]; // DQT 缓存 (info_byte + 64/128 字节/表)
    u_int16_t fQuantTablesLen;
    u_int8_t  fQuantPrecision;
    bool      fJpegParamsValid; // 是否已从首帧解析出 JPEG 参数
};

#endif
