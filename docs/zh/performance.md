# 性能与压测

## 三核 NPU 扩展性（`YOLO_BENCH` 压测）

用合成帧以最大速率驱动推理池，不受摄像头 30fps 限制：
```bash
cd firmware
YOLO_NPU_INSTANCES=1 YOLO_BENCH=300 ./build/bin/app   # 单核
YOLO_NPU_INSTANCES=3 YOLO_BENCH=600 ./build/bin/app   # 三核
```

实测（RK3588S）：

| 模型 | 单实例(1 核) | 三实例(3 核) | 加速比 |
|---|---|---|---|
| yolov8n | ~37–43 fps | ~122–126 fps | ~3× |
| yolov8s | ~28 fps | ~70–78 fps | ~2.7× |

> 数值随 NPU 调频器波动；跑前 `sudo scripts/perf_setup.sh` 锁 performance 更稳定。

## 视频帧率（真实链路，1080p@30）

| 配置 | 视频推流 | 说明 |
|---|---|---|
| yolov8n 单核（改造前，耦合） | 30fps | 恰好跟上 |
| yolov8s 单核（改造前，耦合） | ~25.5fps ⬇ | 推理拖慢视频、掉帧 |
| yolov8s 三核（改造后，解耦） | 30fps ✓ | 视频满帧、检测零掉帧 |

核心收益：**解耦**让视频恒定 30fps（不被推理拖累），**三核池**让更重的模型也能满帧检测。

## 佐证三核占用

跑起来后（需 root）：
```bash
watch -n1 cat /sys/kernel/debug/rknpu/load
# 例：Core0 62%  Core1 60%  Core2 61%
```

## 影响性能的开关
- `YOLO_NPU_INSTANCES`：实例/worker 数（三核用 3）。
- `YOLO_DETECT_STRIDE`：每 N 帧检测一次；很重的模型可设 2 降负载。
- `scripts/perf_setup.sh`：锁频。
- 模型选择：yolov8n（快）/ yolov8s（更准更重）。
