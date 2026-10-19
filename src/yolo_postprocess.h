#ifndef __YOLO_POSTPROCESS_H__
#define __YOLO_POSTPROCESS_H__

/* ============================================================================
 *  YOLOv8 后处理（自 rknn_model_zoo examples/yolov8/cpp/postprocess.{h,cc} 移植）
 *  只保留 RK3588 / RKNPU2 需要的分支：量化(int8) 与 浮点(fp32)。
 *  已裁掉 RV1106/RV1103 与 RKNPU1 的专用分支。
 * ==========================================================================*/

#include <stdbool.h>
#include "rknn_api.h"
#include "common.h"   /* image_rect_t 与绘制库共用一份定义，避免重复 typedef */

#define OBJ_NAME_MAX_SIZE 64
#define OBJ_NUMB_MAX_SIZE 128
#define OBJ_CLASS_NUM     80
#define NMS_THRESH        0.45f
#define BOX_THRESH        0.25f

/* 模型上下文 */
typedef struct {
    rknn_context           rknn_ctx;
    rknn_input_output_num  io_num;
    rknn_tensor_attr      *input_attrs;
    rknn_tensor_attr      *output_attrs;
    int   model_channel;
    int   model_width;
    int   model_height;
    bool  is_quant;
} rknn_app_context_t;

/* letterbox 参数（用于把检测框坐标反算回原图） */
typedef struct {
    int   x_pad;
    int   y_pad;
    float scale;
} letterbox_t;

typedef struct {
    image_rect_t box;
    float        prop;
    int          cls_id;
} object_detect_result;

typedef struct {
    int                  id;
    int                  count;
    object_detect_result results[OBJ_NUMB_MAX_SIZE];
} object_detect_result_list;

/* 加载类别名表（label_path 可为 NULL，此时只用 id） */
int   init_post_process(const char *label_path);
void  deinit_post_process(void);
char *coco_cls_to_name(int cls_id);

/* outputs 传 rknn_output* 数组；letter_box 传本帧的 letterbox 参数 */
int post_process(rknn_app_context_t *app_ctx, void *outputs, letterbox_t *letter_box,
                 float conf_threshold, float nms_threshold,
                 object_detect_result_list *od_results);

#endif /* __YOLO_POSTPROCESS_H__ */
