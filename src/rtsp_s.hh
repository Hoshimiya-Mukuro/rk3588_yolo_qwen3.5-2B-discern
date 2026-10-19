#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include "compat.h"

extern volatile int g_running;
void rtsp_server_start(int port, const char* path, spsc_ring* rb, int enc_type);

#endif