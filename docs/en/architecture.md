# Architecture

## End-to-end data flow

```
V4L2 capture(dma-buf) → MPP decode(MJPEG→NV12) → RGA(CSC/Copy) → burn-in latest boxes → MPP H.264 → ZLMediaKit(RTSP/WebRTC)
                                                        │ async snapshot                 ▲ latest boxes
                                                        ▼                                │
                                              NpuDetectorPool (3-core parallel) ──────────┘
```

- Frames pass between capture/decode/RGA/encode as **dma-buf fds** (zero-copy where possible).
- MJPEG input goes through MPP hardware decode; non-MJPEG (e.g. this board's MIPI-ISP UYVY) takes a raw passthrough path.

## Thread model (firmware)

`run_pipeline()` spawns 6 worker threads connected by queues + mutexes + condition variables:

| Thread | Role |
|---|---|
| capture | V4L2 capture, buffer recycling |
| decode | MJPEG→MPP decode / raw passthrough |
| mpp_output_recycle | recycle decoder output buffers |
| rga | RGA transform + burn-in latest boxes + async submit snapshot |
| encode_input | feed the MPP encoder |
| encode_output | pull H.264 packets, assemble AUs, feed ZLM |

## Inference decoupled from video (core design)

`NpuDetectorPool` ([firmware/src/npu_detector_pool.cpp](../../firmware/src/npu_detector_pool.cpp)):

- **Multi-instance**: instance 0 loads via `rknn_init`; instances 1..N use `rknn_dup_context` to **share
  weights** (saves memory), each pinned to `RKNN_NPU_CORE_0/1/2` via `rknn_set_core_mask`.
- **One instance per worker**: worker k only uses `models_[k]`, so each instance's internal buffers are
  never touched concurrently — avoiding the data race a per-task round-robin would risk.
- **Decoupled**: the video thread calls `GetLatest()` each frame to burn in the most recent detections, and
  `Submit()`s an NV12 snapshot at a sampling cadence (async). Video never waits on inference → steady 30fps.

### Buffer lifecycle

- Detection snapshots are **owned by the pool** (a ring of depth = instances×2). On submit the RGA output is
  memcpy'd into a snapshot and the big buffer is recycled immediately, avoiding a race between async
  inference and encode recycling.
- librga's virtual-address path deadlocks under concurrent calls, so `Preprocess` serializes the RGA
  resize/cvtcolor with a mutex; `rknn_run` still runs in parallel across cores (preprocess is cheap).

## Control plane

- `AppController` ([firmware/src/app_controller.cpp](../../firmware/src/app_controller.cpp)) singleton:
  camera enumeration/switching, YOLO toggle, status, `/api/*` routing.
- `ZlmPublisher` reuses the embedded HTTP (8000) `on_mk_http_request` hook to route `/api/*` to the
  controller; WebRTC negotiation uses `/index/api/webrtc`.
- **Restartable pipeline**: `main()` holds a persistent `ZlmPublisher` + `NpuDetectorPool` across restarts
  and loops `run_pipeline(device)`. Switching cameras = stop current pipeline, rebuild with the new device;
  ZLM/NPU pool are not restarted (stream URL stays). Transient camera failures back off and retry instead of exiting.

## Platform

Zero-dependency Node backend: serves `web/` statically and reverse-proxies `/board/*` to the board so the
frontend is same-origin (no CORS). WebRTC media flows directly board↔browser via ICE. See
[platform/README.md](../../platform/README.md).
