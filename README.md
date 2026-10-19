# nc2rtsp

USB 摄像头 → **RTSP 推流**（H.264 硬编）+ **YOLOv8 实时识别**（RKNN NPU）+ **VLM 多模态联动**（Qwen2-VL / RKLLM）。

面向 **RK3588（LubanCat 系列）/ aarch64** 的嵌入式视频链路，纯 C/C++11。

```
摄像头(UVC/MJPEG) ──> NV12 ──> [YOLOv8 旁路识别] ──> [检测框叠加] ──> MPP H.264 硬编 ──> Live555 ──> RTSP 推流
                                    │ 检测到人(≥0.6) 每 2s
                                    ▼
                          裁剪人像 ──> PPM 落盘 ──> UDP 通知 ──> VLM 进程(独立) 深度推理
```

## 特性

- **1080p / 30fps** 稳定推流，`rtsp://<IP>:8554/live` 直接可拉
- MPP **H.264 硬编码**（异步 split API），码率 4M / GOP 5
- **YOLOv8 旁路识别**：独立 AI 线程，忙则丢帧，**不拖慢推流**（实测 yolov8n ≈ 45.9fps，RTSP drop=0）
- 推流画面上实时叠加**绿色检测框 + 类别标签**（`person 0.87`），只画 person
- **VLM 联动**：检测到人 → 裁剪出人像特写 → 通知独立的多模态进程做深度推理（干净图、无框）
- 画框/识别/联动**全部旁路**：任一个挂了都不影响推流

## 1. 系统要求

| 项     | 要求                                                                        |
| ----- | ------------------------------------------------------------------------- |
| 目标平台  | RK3588（LubanCat），Linux aarch64                                            |
| 交叉工具链 | `/opt/gcc-arm-15.2/bin/aarch64-none-linux-gnu-`（`build.sh` 默认）或板端原生 `gcc` |
| 摄像头   | UVC 摄像头，`/dev/video0`，**MJPEG** 输出（YUYV 通路未实现）                            |
| 板端动态库 | 已随项目提供在 `lib/`（MPP / RGA / RKNN / DRM / vpu）                              |
| 模型文件  | `model/yolov8n.rknn` + `model/coco_80_labels_list.txt`（已提供）               |

> ⚠️ **模型路径默认是 `yolov8s.rknn`，仓库里只放了 `yolov8n.rknn`**。运行前要么把 `yolov8s.rknn` 拷进 `model/`，要么改 `main.cpp:25` 的 `YOLO_MODEL_PATH` 指向 `model/yolov8n.rknn`（见 §5）。

## 2. 目录结构

```
nc2rtsp/
├── main.cpp                  # 入口：初始化 + 采集线程 + 启动 RTSP
├── build.sh                  # 一键构建（make/cmake、交叉/原生）
├── Makefile / CMakeLists.txt # 两套构建（用 build.sh 统一入口）
├── compat.h                  # C/C++ 混编桥梁（extern "C"）
├── src/
│   ├── uvc.c/.h              # V4L2 采集（MJPEG，分辨率协商）
│   ├── decode.c/.h           # TurboJPEG MJPEG→NV12 + RGA NV12→RGB888
│   ├── encode.c/.h           # MPP H.264 硬编 + YOLO 投递 + 检测框叠加 + stride 重排
│   ├── ringbuf.c/.h          # SPSC 无锁环形缓冲（H.264 帧队列）
│   ├── devicesource.cpp/.hh  # Live555 数据源（拆裸 NAL + 剥 start code）
│   ├── rtsp_s.cpp/.hh        # RTSP 服务器装配
│   ├── yolo.cc/.h            # YOLOv8 RKNN 识别（独立 AI 线程 + 最新帧邮箱）
│   ├── yolo_postprocess.cc/.h# YOLOv8 后处理（移植自 rknn_model_zoo）
│   ├── vlm_link.c/.h         # VLM 联动：时间闸门 + 裁剪人像 + PPM 落盘 + UDP
│   ├── image_drawing.c/.h    # NV12 画框/画字（移植自 rknn_model_zoo，含 font.h）
│   ├── common.h / font.h     # 绘制库依赖 + 字模数据
│   └── test_time.h           # LOG_TIME 编译期打点
├── include/                  # 第三方头：jpeg(turbojpeg) / live555 / rknn
├── lib/                      # 板端运行库（.so/.a + rk3588_libs.tar.gz）
├── model/                    # yolov8n.rknn + coco 标签表
└── cmake/                    # CMake 交叉工具链文件
```

## 3. 编译

```bash
./build.sh                          # 默认：aarch64 交叉编译（Makefile），产物 build/bin/nc2rtsp
./build.sh -c                       # 先 clean
./build.sh -t native                # 板端原生编译（host gcc）
./build.sh -t /path/to/aarch64-linux-gnu- -c   # 指定工具链
./build.sh -b cmake                 # 改用 CMake
./build.sh -m                       # 编译期启用 MPP（-DENABLE_MPP）
./build.sh -d                       # Debug（-O0 -g3）
./build.sh -j 8                     # 并行
```

- 换工具链/环境后**务必 `make clean` 或 `-c`**，否则残留旧 `.o` 会报莫名链接错误。
- 若链接报"找不到 `-lrga` / `-lrockchip_mpp`"：`lib/` 下是带版本号的 `.so`，先执行 `make libs` 生成无版本号软链（幂等）。
- 打点日志默认关闭；需要排查时序时加 `-DNC2RTSP_ENABLE_LOG_TIME`（见 §10）。

## 4. 运行

把整个工程目录（或至少 `build/bin/nc2rtsp` + `lib/` + `model/`）拷贝到板子，然后：

```bash
cd nc2rtsp
export LD_LIBRARY_PATH=./lib        # MPP/RGA/RKNN 等 .so 都在项目 lib/ 下
./build/bin/nc2rtsp
```

预期日志：

```
[MAIN] 实际采集参数: 1920x1080, pixfmt=0x...   (MJPEG)
[VLM] link up: udp 127.0.0.1:9600, ppm /dev/shm/vlm_frame.ppm, interval 2000ms, trigger cls=0 conf>=0.60, keep 3
[YOLO] init ok, model=model/yolov8n.rknn ...
[YOLO] stat: infer=2130 drop=0 avg=21.8ms (45.9fps)    # 每 30 帧一条
```

**拉流查看**（VLC / ffplay）：

```bash
ffplay rtsp://<板子IP>:8554/live
```

帧率 30fps 下推流端 `drop=0` 是正常的；若 YOLO 忙，`[YOLO] stat` 的 `drop` 计数会上升（AI 线程丢帧，不影响推流）。

## 5. 配置（都在 `main.cpp` 硬编码）

当前入口**无命令行参数**，需要改配置直接改宏：

| 项             | 位置                                                     | 默认                                        |
| ------------- | ------------------------------------------------------ | ----------------------------------------- |
| 摄像头设备         | `main.cpp:74`                                          | `/dev/video0`                             |
| 期望分辨率         | `REQ_WIDTH / REQ_HEIGHT`（`main.cpp:21-22`）             | 1920×1080（驱动不支持会自动降级）                     |
| 帧率            | `main.cpp:74`（`uvc_init`）                              | 30                                        |
| YOLO 模型 / 标签表 | `YOLO_MODEL_PATH / YOLO_LABEL_PATH`（`main.cpp:25-26`）  | `model/yolov8s.rknn` ⚠️                   |
| 码率 / GOP      | `main.cpp:83-84`                                       | 4M / 5                                    |
| VLM 联动目标      | `VLM_UDP_IP / PORT / PPM_PATH`（`main.cpp:29-31`）       | `127.0.0.1:9600`、`/dev/shm/vlm_frame.ppm` |
| 送图间隔 / 触发置信度  | `VLM_INTERVAL_MS / VLM_TRIGGER_CONF`（`main.cpp:32-33`） | 2000ms / 0.60                             |
| 触发类别          | `trigger_cls[]`（`main.cpp:112`）                        | `{0}`（COCO person）                        |
| RTSP 端口 / 流名  | `main.cpp:134`（`rtsp_server_start`）                    | 8554 / `live`                             |

## 6. 架构与数据流

```
                       ┌────────────── 采集线程（capture_thread）──────────────┐
 /dev/video0 ── uvc_read ──> MJPEG ──> mjpeg_to_nv12 ──> 紧凑NV12(g_packed_buf)
                                              │
                       ┌──────────────────────┼──────────────────────┐
                       │ yolo_publish_frame()  │ encoder_draw_overlay │  packed_to_strided()
                       │ （旁路投递，干净帧）    │ （画绿框+标签）        │  （紧凑→MPP stride）
                       ▼                      ▼                      ▼
              ┌── AI 线程（独立）──┐     送 MPP 的这一份带框         MPP H.264 硬编
              │ acquire(2s闸门)   │                                  │
              │ RGA 全分辨率RGB    │                             spsc_write
              │ letterbox→640×640 │                                  ▼
              │ rknn_run 推理      │                            H264DeviceSource
              │ 后处理→g_result    │                           （拆裸NAL/剥start code）
              │ commit:命中person? │                                  ▼
              │  裁剪人像          │                            Live555 → RTP
              │  PPM落盘(seq命名)  │                                  ▼
              │  UDP通知VLM        │                            RTSP :8554/live
              └──────────────────┘
```

三个关键设计：

1. **全部旁路**：YOLO、画框、VLM 都是"主链路上的旁路"，任何一个失败只打日志、推流照常。
2. **画框放在投递之后**：VLM 拿到干净帧、YOLO 看不见自己的框（避免自检反馈环）、不把 22ms 推理塞进编码链路。
3. **通知与数据绑定**：落盘按 `seq` 命名（一帧一文件，只留 3 张），UDP 报文里的路径唯一对应那一帧，杜绝"VLM 读到的不是被通知的那帧"。

## 7. 模块说明

| 模块                 | 职责               | 关键点                                                                                          |
| ------------------ | ---------------- | -------------------------------------------------------------------------------------------- |
| `uvc.c`            | V4L2 采集          | MJPEG 优先协商；驱动降级时用回读的实际分辨率                                                                    |
| `decode.c`         | MJPEG→NV12 + RGA | TurboJPEG 句柄复用；NV12→RGB888 用 RGA（letterbox/resize/整帧）                                        |
| `encode.c`         | MPP 硬编           | **MppEncCfg key-value 配置**（废弃 struct 接口会静默失效）；异步 split API；`packed_to_strided` 修复 1080p 色度错位 |
| `ringbuf.c`        | SPSC 队列          | 无锁，忙则丢最新帧保实时                                                                                 |
| `devicesource.cpp` | Live555 源        | **拆整包为裸 NAL、剥 start code**（DiscreteFramer 契约）                                                |
| `yolo.cc`          | AI 识别            | 独立线程 + 最新帧邮箱 + 忙则丢；结果分"消费式"与"快照式"两个出口                                                        |
| `vlm_link.c`       | VLM 联动           | 时间闸门 + 置信度门槛 + 裁剪人像 + 一帧一文件 + UDP                                                            |
| `image_drawing.*`  | NV12 绘制          | 从 rknn_model_zoo 整份移植；坐标/字号必须偶数                                                              |

## 8. VLM 联动

**联动协议**（UDP，默认 `127.0.0.1:9600`，由本程序发送）：

```
VLM1|<seq>|<path>|<W>x<H>|<crop_x>,<crop_y>|<dets>
```

- `path`：本次裁剪图的唯一路径，如 `/dev/shm/vlm_frame_603.ppm`（保留最近 3 张）
- `W/H`：裁剪图尺寸；`crop_x/crop_y`：裁剪区在原图中的左上角
- `dets`：`cls:conf:x1,y1,x2,y2;...`（坐标仍是**原图坐标系**，减 `crop_x/y` 即得裁剪图坐标）

**触发条件**：每 2s 一次时间闸门 + YOLO 命中 person（COCO 0，置信度 ≥ 0.60）→ 以人像为中心裁正方形（边长 = 人框长边 × 2，外扩留上下文）→ 原分辨率 PPM 落盘 → UDP 通知。

**VLM 侧**是独立的 Rockchip 多模态工程（`rknn-llm-main/examples/multimodal_model_demo`，Qwen2-VL + RKLLM），不在本仓库内。它常驻监听 9600 端口，收到通知后读 `path` 指定的图做深度推理，输出用 `<VLM_OUT_BEGIN>/<VLM_OUT_END>` 围栏包裹。**VLM 侧只需按协议改其 `deploy/src/main.cpp`，本程序零依赖。**

不跑 VLM 也能先验证联动通道：

```bash
ls -l /dev/shm/vlm_frame_*.ppm          # 应只有最近 3 张
# 手动发一包（路径换成实际存在的文件）
python3 -c "import socket; s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM); \
s.sendto(b'VLM1|1|/dev/shm/vlm_frame_603.ppm|1080x1080|0,0|person:0.87:0,0,100,100',('127.0.0.1',9600))"
```

## 9. 性能（实测参考）

| 指标            | 值                          | 说明                     |
| ------------- | -------------------------- | ---------------------- |
| 推流            | 1920×1080 @ 30fps，`drop=0` | 编码约 8–15ms/帧（33ms 预算内） |
| YOLO（yolov8n） | ≈ 21.8ms/帧 → **45.9fps**   | 独立 AI 线程，忙则丢帧          |
| 检测框叠加         | ≈ 0.1–0.5ms/帧              | 对 30fps 推流无感           |
| VLM 联动开销      | 无人在画面时 ≈ 0                 | 时间闸门未到不做全分辨率转换         |

> 数值与模型/板子/CPU 负载相关，仅供参考。

## 10. 常见问题排查

| 现象                                       | 原因与排查                                                                                                                       |
| ---------------------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| 推流**花屏/条纹/画面正中分割线**                      | 编码器配置没生效。看 SPS 的 `level_idc`：<40 说明配置失败。确认用了 `MppEncCfg`（废弃 struct 接口会静默失效），且 `encoder_init` 用的是**协商后**的 `g_width/g_height` |
| **1080p 偏色/底部彩噪**（720p 正常）               | NV12 stride：`packed_to_strided` 必须把紧凑 NV12 重排成 MPP 的 `ALIGN(h,16)` 布局                                                       |
| 刷屏 `MPEG 'start code' seen in the input` | 给 DiscreteFramer 喂了带 start code 的包——数据源必须拆裸 NAL 并剥 start code                                                               |
| `[YOLO] init fail`                       | `model/` 下没有 `main.cpp` 里 `YOLO_MODEL_PATH` 指向的文件（默认 `yolov8s.rknn`，仓库只放了 `yolov8n.rknn`）。拷模型或改宏                            |
| 链接报 `undefined reference`                | C/C++ 混编头文件漏 `extern "C"`；或 `lib/` 缺无版本号软链（`make libs`）                                                                     |
| 换工具链后链接报错                                | 没 clean，残留旧 `.o`。`./build.sh -c`                                                                                            |
| 采集不到帧 / 分辨率不对                            | 摄像头不是 MJPEG 输出（YUYV 通路未实现）；驱动降级时看 `[MAIN] 实际采集参数` 日志                                                                        |

## 11. 注意事项
孩子们，真拿了我的代码去用记得点个star，顺便帮我宣传一下，虽然也未必有人看得上


