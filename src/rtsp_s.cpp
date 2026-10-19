#include "rtsp_s.hh"

#include <stdio.h>
#include <signal.h>

#include "test_time.h"
#include "devicesource.hh"
#include "liveMedia.hh"
#include "BasicUsageEnvironment.hh"
#include "GroupsockHelper.hh"
#include "H264VideoStreamDiscreteFramer.hh"

class DeviceSourceSubsession: public OnDemandServerMediaSubsession
{
public:
    static DeviceSourceSubsession* creatnew(UsageEnvironment& env, spsc_ring* rb, int encType)
    {
        return new DeviceSourceSubsession(env, rb, encType);
    }

protected:
    DeviceSourceSubsession(UsageEnvironment& env, spsc_ring* rb, int encType)
    : OnDemandServerMediaSubsession(env, False /*reuseFirstSource*/), fRingbuf(rb), fEncType(encType) 
    {
    }

    virtual FramedSource* createNewStreamSource(unsigned /*clientSessionId*/, unsigned& estBitrate)
    {
        estBitrate = 4000;   //kbps
        if (fEncType == ENC_MPP) {
            FramedSource* es = H264DeviceSource::createNew(envir(), fRingbuf);
            /* includeStartCodeInOutput 必须保持默认的 False：
             * 数据源已经把 MPP 包拆成裸 NAL（无 start code）逐个送出，
             * framer 不能再补 start code，否则 RTP 载荷首字节变成 0x00 → 花屏。 */
            return H264VideoStreamDiscreteFramer::createNew(envir(), es);
        } else {
            return MJPEGDeviceSource::createNew(envir(), fRingbuf);
        }
    }

    virtual RTPSink* createNewRTPSink(Groupsock* rtpGS, unsigned char rtpPayloadType, FramedSource* /*inputSource*/)
    {
        // MJPEG 帧最大 ~750KB，默认 OutPacketBuffer 只有 60KB → 必须提前扩大
        OutPacketBuffer::increaseMaxSizeTo(5 * 1024 * 1024);

        if (fEncType == ENC_MJPEG)
        {
            /* JPEGVideoRTPSink 自动构造 RFC 2435 RTP 头（含量化表等），
             * VLC / ffplay 必须此格式才能解码 */
            return JPEGVideoRTPSink::createNew(envir(), rtpGS);
        }
        if (fEncType == ENC_MPP)
        {
            EncoderSpsPps sps_pps;
            encoder_get_sps_pps(&sps_pps);
            return H264VideoRTPSink::createNew(envir(), rtpGS, rtpPayloadType,
                sps_pps.sps, sps_pps.sps_len,
                sps_pps.pps, sps_pps.pps_len);
        } 
        
    }
private:
    spsc_ring* fRingbuf;
    int fEncType;
   
};



struct WatchData 
{
    RTSPServer* server;
    UsageEnvironment* env;
};

static void sigint_handler(int /*sig*/)
{
    g_running = 0;
}

void rtsp_server_start(int port, const char* path, spsc_ring* rb, int enc_type)
{
    TaskScheduler* scheduler = BasicTaskScheduler::createNew();
    UsageEnvironment* env = BasicUsageEnvironment::createNew(*scheduler);
    LOG_TIME("RTSP: scheduler/env 创建完成");
    RTSPServer* rtsp_ser = RTSPServer::createNew(*env, port, NULL);
    if(!rtsp_ser)
    {
        *env << "[RTSP] Failed to create server on port " << port << "\n";
        return;
    }  
    LOG_TIME("RTSP: RTSPServer 创建完成");
    WatchData* data = new WatchData{rtsp_ser, env};

    ServerMediaSession* sms = ServerMediaSession::createNew(*env, path, "USB Camera Live", "USB Camera RTSP stream");
    sms->addSubsession(DeviceSourceSubsession::creatnew(*env, rb, enc_type));
    rtsp_ser->addServerMediaSession(sms);
    LOG_TIME("RTSP: 添加 SMS 到 Server 完成");
    char* url = rtsp_ser->rtspURL(sms);
    *env << "[RTSP] ==================================\n";
    *env << "[RTSP] " << url << "\n";
    *env << "[RTSP] Type: "
         << (enc_type == ENC_MPP ? "H.264 (MPP)" : "MJPEG") << "\n";
    *env << "[RTSP] ==================================\n";
    delete[] url;

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    class ExitWatcher 
    {
    public:
        static void watch(void* clientData)
        {
            WatchData* data = (WatchData*)clientData;
            if (!g_running) 
            {
                Medium::close(data->server);
                delete data;
                return;
            }
            data->env->taskScheduler().scheduleDelayedTask(500000, (TaskFunc*)watch, clientData);
        }
    };
    scheduler->scheduleDelayedTask(500000, (TaskFunc*)ExitWatcher::watch, data);
    LOG_TIME("RTSP: 退出监控任务已调度");
    env->taskScheduler().doEventLoop();
    *env << "[RTSP] Server stopped.\n";
}



