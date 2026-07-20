# EdgeVision-RK3588 · firmware

板端程序（C++17）：`V4L2 采集 → MPP 解码 → RGA → YOLO NPU（三核并行）→ MPP H.264 → ZLMediaKit(RTSP/WebRTC)` + `/api/*` 控制接口。

On-board program: full capture→inference→encode→stream pipeline with triple-NPU YOLO and a control API.

## 快速开始 / Quick start
```bash
cmake -S . -B build && cmake --build build -j$(nproc)
# 模型放到 model/（见 model/README.md）
CAMERA_EXCLUDE=/dev/video12 ./build/bin/app
```

详见根目录文档 / see the top-level docs:
[`docs/zh`](../docs/zh) · [`docs/en`](../docs/en)（架构 / 编译 / 使用 / 接口 / 性能 / 排障）。

## 源码结构 / Source layout
```
src/
├── main.cpp               主循环 + 可重启流水线 run_pipeline
├── v4l2Camera.cpp         V4L2 采集
├── rkmppdec.cpp           MPP 解码
├── rkmppenc.cpp           MPP H.264 编码（Baseline，WebRTC 兼容）
├── rkrga.cpp              RGA 图像处理
├── yolo_npu.cpp           单实例 YOLO（dup_context/core_mask）
├── npu_detector_pool.cpp  多实例 NPU 池 + 解耦推理 + 压测
├── app_controller.cpp     控制器：摄像头枚举/切换、YOLO 开关、状态、/api 路由
├── nv12_overlay.cpp       NV12 检测框软件烧录
└── zlm_publisher.cpp      推流 + /api/* HTTP 钩子
include/                   对应头文件 + common_utils.h
model/                     yolov8n.rknn + coco_80_labels_list.txt（不入库）
scripts/perf_setup.sh      NPU/CPU 锁频
lib/                       ZLMediaKit 预编译库
```
