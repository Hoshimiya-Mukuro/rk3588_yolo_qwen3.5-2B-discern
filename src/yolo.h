#ifndef __YOLO_H__
#define __YOLO_H__

/* ============================================================================
 *  YOLOv8 (RKNN / RK3588 NPU) 识别模块
 *
 *  用法（C / C++ 均可调用，实现是 C++）：
 *      1) 启动：  yolo_start("model/yolov8s.rknn", "model/coco_80_labels_list.txt");
 *      2) 旁路：  在生成紧凑 NV12 之后调用 yolo_publish_frame(nv12, w, h);
 *                 —— 非阻塞，AI 线程忙时直接丢帧，绝不拖慢采集/推流
 *      3) 取结果：yolo_get_latest_result(&res);  （消费一次，返回 1 表示有新结果）
 *      4) 退出：  yolo_stop();
 *
 *  推理在独立线程里做：NV12 --RGA--> RGB888(letterbox) --NPU--> 后处理 --> 结果
 * ==========================================================================*/

#ifdef __cplusplus
extern "C" {
#endif

#define YOLO_MAX_OBJECTS 128

typedef struct {
    int   cls_id;                    /* 类别 id（0..79，COCO） */
    float prop;                      /* 置信度 0..1 */
    int   left, top, right, bottom;  /* 相对**原始采集帧**的像素坐标 */
} yolo_object_t;

typedef struct {
    int           count;                          /* 目标个数 */
    yolo_object_t objects[YOLO_MAX_OBJECTS];
    int           frame_w, frame_h;               /* 该结果对应的原始帧尺寸 */
    long long     seq;                            /* 帧序号（自增） */
} yolo_result_t;

/* 加载模型 + 标签表，并启动识别线程。
 * model_path: .rknn 文件路径（如 "model/yolov8s.rknn"）
 * label_path: 类别名列表（如 "model/coco_80_labels_list.txt"），可为 NULL
 * 返回 0 成功，-1 失败（失败时推流照常，只是没有 AI 结果）。 */
int yolo_start(const char *model_path, const char *label_path);

/* 停止识别线程并释放模型 */
void yolo_stop(void);

/* 投递一帧**紧凑** NV12（stride == width，即 mjpeg_to_nv12 的输出）。
 * 非阻塞：AI 线程还在处理上一帧时直接返回 0（丢帧），保证采集线程不被拖住。
 * 返回 1 表示已接收。 */
int yolo_publish_frame(const void *nv12, int width, int height);

/* 取最近一次识别结果。返回 1 表示拿到新结果（并清掉“新”标记），0 表示暂无新结果。 */
int yolo_get_latest_result(yolo_result_t *out);

/* 只读快照：**不清**“新”标记，专供画框等旁路使用。
 * 返回 1 表示有可用结果（可能是上一帧的，最多滞后一帧）。 */
int yolo_snapshot(yolo_result_t *out);

/* 类别 id → 名字（未加载标签表时返回 "null"） */
const char *yolo_cls_to_name(int cls_id);

#ifdef __cplusplus
}
#endif

#endif /* __YOLO_H__ */
