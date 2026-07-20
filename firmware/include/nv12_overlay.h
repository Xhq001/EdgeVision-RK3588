#ifndef NV12_OVERLAY_H
#define NV12_OVERLAY_H

#include "pubDataType.h"
#include "yolo_npu.h"

#include <functional>
#include <string>
#include <vector>

// NV12 帧上的软件叠加渲染：把 YOLO 检测框 + 类别/分数标签直接烧录进 NV12 图像
// （内部含点阵字库与画框/画字实现，均为纯 CPU 逐像素操作）。
// label_of: 根据 cls_id 返回类别名的回调（通常来自 NpuDetectorPool::GetLabelName）。
void draw_yolo_boxes_on_nv12(const IO_FD_t* frame,
                             const std::vector<YoloNpuInstance::DetectBox>& boxes,
                             const std::function<std::string(int)>& label_of);

#endif  // NV12_OVERLAY_H
