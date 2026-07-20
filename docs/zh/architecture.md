# 架构设计

## 全链路数据流

```
V4L2 采集(dma-buf) → MPP 解码(MJPEG→NV12) → RGA(CSC/Copy) → 烧录最近检测框 → MPP H.264 编码 → ZLMediaKit(RTSP/WebRTC)
                                                  │ 异步提交快照                    ▲ 最近检测框
                                                  ▼                                │
                                        NpuDetectorPool（三核并行推理）────────────┘
```

- 采集/解码/RGA/编码之间以 **dma-buf fd** 传递帧，全链路尽量零拷贝。
- MJPEG 输入走 MPP 硬件解码；非 MJPEG（如本项目 MIPI ISP 的 UYVY）走 raw 直通路径。

## 线程模型（firmware）

`run_pipeline()` 内起 6 个工作线程，用队列 + 互斥锁 + 条件变量衔接：

| 线程 | 职责 |
|---|---|
| capture | V4L2 采集、回收 buffer |
| decode | MJPEG→MPP 解码 / raw 直通 |
| mpp_output_recycle | 回收解码输出 buffer |
| rga | RGA 转换 + 烧录最近检测框 + 异步提交检测快照 |
| encode_input | 送 MPP 编码器 |
| encode_output | 取 H.264 包、组 AU、送 ZLM |

## 推理与视频解耦（核心设计）

`NpuDetectorPool`（[firmware/src/npu_detector_pool.cpp](../../firmware/src/npu_detector_pool.cpp)）：

- **多实例**：实例 0 用 `rknn_init` 加载模型；实例 1..N 用 `rknn_dup_context` **共享权重**（省内存），
  各实例 `rknn_set_core_mask` 绑定 `RKNN_NPU_CORE_0/1/2`。
- **每 worker 独占一个实例**：worker k 只用 `models_[k]`，各实例内部缓冲天然无并发访问，
  规避了"按任务 round-robin 选实例"可能造成的同实例被两线程同时使用的数据竞争。
- **解耦**：视频线程（rga 线程）每帧只调 `GetLatest()` 取最近一次完成的检测框并烧录，
  按采样节奏 `Submit()` 一份 NV12 快照给池（异步）。视频**不等待推理**，恒定 30fps。

### buffer 生命周期

- 检测快照由池**独立拥有**（独立 ring，深度 = 实例数×2），提交时把 RGA 输出 memcpy 进快照后立即回收大 buffer，
  避免异步推理与编码回收竞争同一 buffer。
- librga 的 virtual-address 路径在多线程并发调用会死锁；`Preprocess` 里用一把互斥锁串行化 RGA(resize/cvtcolor)，
  `rknn_run` 仍在各核并行（前处理很轻，串行化几乎不影响吞吐）。

## 控制平面

- `AppController`（[firmware/src/app_controller.cpp](../../firmware/src/app_controller.cpp)）单例：摄像头枚举/切换、
  YOLO 开关、状态汇总、`/api/*` 路由。
- `ZlmPublisher` 复用内嵌 HTTP(8000) 的 `on_mk_http_request` 钩子，把 `/api/*` 转给 controller；WebRTC 协商走 `/index/api/webrtc`。
- **可重启流水线**：`main()` 持有跨重启保活的 `ZlmPublisher` + `NpuDetectorPool`，循环调用 `run_pipeline(device)`；
  切摄像头 = 停当前流水线、用新设备重建，ZLM/NPU 池不重启（流地址不变）。相机瞬时故障会退避重启重试而非退出。

## 平台（platform）

Node 零依赖后端：静态托管 `web/` + 把 `/board/*` 反向代理到开发板，前端全部同源请求（免跨域）；
WebRTC 媒体由开发板经 ICE 直连浏览器。详见 [platform/README.md](../../platform/README.md)。
