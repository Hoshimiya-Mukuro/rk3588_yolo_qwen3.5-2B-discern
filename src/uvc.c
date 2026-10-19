#include "uvc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <linux/videodev2.h>
#include <poll.h>

/*
open → QUERYCAP → S_FMT → G_FMT → S_PARM
     → REQBUFS → QUERYBUF → mmap → QBUF → STREAMON
循环：DQBUF → 处理 → QBUF
结束：STREAMOFF → munmap → close
*/

#define MAX_COUNT 4

int g_fd = -1;
int g_pixfmt = -1;          // V4L2_PIX_FMT_YUYV 或 MJPEG
int g_width = -1;
int g_height = -1;
size_t g_frame_len = 0;     // 每帧最大字节数

struct 
{
    void *start_dir;
    size_t length;
}g_bufs[MAX_COUNT];

static int xioctl(int fd, int req, void *arg)
{
    int r;
    r = ioctl(fd, req, arg);
    while (r == -1 && errno == EINTR)
    {
        r = ioctl(fd, req, arg);
    }
    return r;
}

static bool set_format(uint32_t pixfmt)
{
    if (g_width <= 0 || g_height <= 0)
    {
        return false;
    }

    struct v4l2_format v_fmt = {0};
    v_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v_fmt.fmt.pix.width = g_width;
    v_fmt.fmt.pix.height = g_height;
    v_fmt.fmt.pix.pixelformat = pixfmt;
    v_fmt.fmt.pix.field = V4L2_FIELD_NONE;
    v_fmt.fmt.pix.priv = 1;

    if (xioctl(g_fd, VIDIOC_S_FMT, &v_fmt) < 0)
    {
        return false;
    }
    if (v_fmt.fmt.pix.pixelformat != pixfmt)
    {
        /* 驱动不支持该像素格式（此时原格式保持不变） */
        return false;
    }

    /* 接受驱动协商后的实际分辨率，不能把请求值当成结果 */
    int new_w = (int)v_fmt.fmt.pix.width;
    int new_h = (int)v_fmt.fmt.pix.height;
    if (new_w != g_width || new_h != g_height)
    {
        printf("[UVC] driver adjusted resolution: %dx%d -> %dx%d\n",
               g_width, g_height, new_w, new_h);
    }
    g_width  = new_w;
    g_height = new_h;
    return true;
}

//============================API=====================================

bool uvc_init(const char *dev, int width, int height, int fps)
{
    g_width    = width;
    g_height   = height;
    g_frame_len = 0;

    g_fd = open(dev, O_RDWR);
    if(g_fd < 0)
    {
        printf("[UVC] open %s: %s\n", dev, strerror(errno));
        goto fail;
    }

    struct v4l2_capability cap ={0};
    if(xioctl(g_fd,  VIDIOC_QUERYCAP, &cap) < 0)
    {
        printf("[UVC] VIDIOC_QUERYCAP: %s\n", strerror(errno));
        goto fail;
    }
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) 
    {
        printf("[UVC] %s is not a capture device\n", dev);
        goto fail;
    }

    /* ── 像素格式协商：优先 MJPEG（当前只有 MJPEG→NV12→H.264 这一条通路）── */
    if (set_format(V4L2_PIX_FMT_MJPEG))
    {
        g_pixfmt = V4L2_PIX_FMT_MJPEG;
        printf("[UVC] set MJPEG\n");
    }
    else if (set_format(V4L2_PIX_FMT_YUYV))
    {
        g_pixfmt = V4L2_PIX_FMT_YUYV;
        printf("[UVC] set YUYV\n");
        printf("[UVC] YUYV→NV12 通路尚未实现，无法编码，退出\n");
        goto fail;
    }
    else
    {
        printf("[UVC] neither MJPEG nor YUYV supported\n");
        goto fail;
    }

    /* ── 回读驱动实际生效的格式（分辨率可能被驱动降级，如 1080p→720p）── */
    struct v4l2_format actual = {0};
    actual.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(g_fd, VIDIOC_G_FMT, &actual) < 0)
    {
        printf("[UVC] VIDIOC_G_FMT: %s\n", strerror(errno));
        goto fail;
    }
    if (actual.fmt.pix.width == 0 || actual.fmt.pix.height == 0)
    {
        printf("[UVC] invalid actual resolution %ux%u\n",
               actual.fmt.pix.width, actual.fmt.pix.height);
        goto fail;
    }
    g_width     = actual.fmt.pix.width;
    g_height    = actual.fmt.pix.height;
    g_frame_len = actual.fmt.pix.sizeimage;

    printf("[UVC] %s: %dx%d %s, frame_len=%zu\n",
              dev, g_width, g_height,
              g_pixfmt == V4L2_PIX_FMT_YUYV ? "YUYV" : "MJPEG",
              g_frame_len);
    
    struct v4l2_streamparm v_parm = {0};
    v_parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    v_parm.parm.capture.timeperframe.numerator = 1;
    v_parm.parm.capture.timeperframe.denominator = fps;
    if(xioctl(g_fd, VIDIOC_S_PARM, &v_parm) < 0)
    {
        //不是所有驱动都支持设帧率
        printf("[UVC] VIDIOC_S_PARM: %s (ignored)\n", strerror(errno));
    }
    else
    {
        /* 回读驱动实际生效的帧率（1080p 下摄像头常把帧率降到 15/20） */
        struct v4l2_streamparm got = {0};
        got.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(g_fd, VIDIOC_G_PARM, &got) == 0 &&
            got.parm.capture.timeperframe.denominator > 0 &&
            got.parm.capture.timeperframe.numerator > 0)
        {
            printf("[UVC] fps: requested %d, actual %u/%u\n", fps,
                   got.parm.capture.timeperframe.denominator,
                   got.parm.capture.timeperframe.numerator);
        }
    }

    struct v4l2_requestbuffers req = {0};
    req.count = MAX_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(g_fd, VIDIOC_REQBUFS, &req) < 0) 
    {
        fprintf(stderr, "[UVC] VIDIOC_REQBUFS: %s\n", strerror(errno));
        goto fail;
    }
    if (req.count < MAX_COUNT) 
    {
        fprintf(stderr, "[UVC] only %u buffers (want %d)\n", req.count, MAX_COUNT);
        goto fail;
    }

    for(int i = 0; i < MAX_COUNT; i++)
    {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i; 

        if (xioctl(g_fd, VIDIOC_QUERYBUF, &buf) < 0) 
        {
            fprintf(stderr, "[UVC] VIDIOC_QUERYBUF[%d]: %s\n", i, strerror(errno));
            goto fail_unmap;
        }

        g_bufs[i].length = buf.length;
        g_bufs[i].start_dir  = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, buf.m.offset);
        if (g_bufs[i].start_dir == MAP_FAILED) 
        {
            printf("[UVC] mmap[%d]: %s\n", i, strerror(errno));
            g_bufs[i].start_dir = NULL;
            goto fail_unmap;
        }
    }

    for (int i = 0; i < MAX_COUNT; i++) 
    {
        struct v4l2_buffer buf = {0};
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;
        if (xioctl(g_fd, VIDIOC_QBUF, &buf) < 0) 
        {
            printf("[UVC] VIDIOC_QBUF[%d]: %s\n", i, strerror(errno));
            goto fail_unmap;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(g_fd, VIDIOC_STREAMON, &type) < 0) 
    {
        printf("[UVC] VIDIOC_STREAMON: %s\n", strerror(errno));
        goto fail_unmap;
    }
    return true;


fail_unmap:
    for (int i = 0; i < MAX_COUNT; i++) 
    {
        if (g_bufs[i].start_dir && g_bufs[i].start_dir != MAP_FAILED) 
        {
            munmap(g_bufs[i].start_dir, g_bufs[i].length);
            g_bufs[i].start_dir = NULL;
        }
    }

fail:
    if (g_fd >= 0) 
    {
        close(g_fd);
        g_fd = -1;
    }
    return false;
}

int uvc_read_frame(UvcFrame *frame)
{
    /*
    setbuf(stdout, NULL); 
    if(g_fd < 0)
    {
        return 0;
    }
    int flags = fcntl(g_fd, F_GETFL, 0);
    fcntl(g_fd, F_SETFL, flags | O_NONBLOCK);

    struct pollfd pfd;
    pfd.fd = g_fd;
    pfd.events = POLLIN;
    int ret = poll(&pfd, 1, 100);
    if (ret < 0) 
    {
        perror("poll");
        return 0;
    } 
    else if (ret == 0) 
    {
        fprintf(stderr, "[UVC] poll timeout\n"); fflush(stderr);
        return 0;
    }

    printf("[UVC] calling DQBUF...\n");  fflush(stdout);
    struct v4l2_buffer vbuf = {0};
    vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(g_fd, VIDIOC_DQBUF, &vbuf) < 0) 
    {
        fprintf(stderr, "[UVC] DQBUF: %s\n", strerror(errno));   fflush(stdout);
        return 0;
    }

    frame->data = g_bufs[vbuf.index].start_dir;
    frame->len = vbuf.bytesused;        
    frame->pts_us = (int64_t)vbuf.timestamp.tv_sec * 1000000 + vbuf.timestamp.tv_usec;
    frame->buf_index = vbuf.index;

    return 1;
    */

    if (g_fd < 0) return 0;

    
    int flags = fcntl(g_fd, F_GETFL, 0);
    fcntl(g_fd, F_SETFL, flags & ~O_NONBLOCK);

    
    struct v4l2_buffer vbuf = {0};
    vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;

    
    fflush(stdout);

    if (xioctl(g_fd, VIDIOC_DQBUF, &vbuf) < 0) 
    {
        perror("[UVC] DQBUF error");
        fflush(stderr);
        return 0;
    }
    
    fflush(stdout);

    frame->data = g_bufs[vbuf.index].start_dir;
    frame->len = vbuf.bytesused;
    frame->pts_us = (int64_t)vbuf.timestamp.tv_sec * 1000000 + vbuf.timestamp.tv_usec;
    frame->buf_index = vbuf.index;
    return 1;
}

int uvc_release_frame(UvcFrame *frame)
{
    if (g_fd < 0 || frame->buf_index < 0)
    {
        return 0;
    }     

    struct v4l2_buffer vbuf = {0};
    vbuf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vbuf.memory = V4L2_MEMORY_MMAP;
    vbuf.index  = frame->buf_index;

    xioctl(g_fd, VIDIOC_QBUF, &vbuf);
    frame->buf_index = -1;  

    return 1;
}


bool uvc_close(void)
{
    if (g_fd < 0)
        return false;

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    xioctl(g_fd, VIDIOC_STREAMOFF, &type);

    for (int i = 0; i < MAX_COUNT; i++) 
    {
        if (g_bufs[i].start_dir && g_bufs[i].start_dir != MAP_FAILED)
        {
            munmap(g_bufs[i].start_dir, g_bufs[i].length);
        }
        g_bufs[i].start_dir = NULL;
    }

    close(g_fd);
    g_fd = -1;
    return true;
}