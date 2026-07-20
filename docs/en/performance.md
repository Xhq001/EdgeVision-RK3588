# Performance & Benchmarking

## 3-core NPU scaling (`YOLO_BENCH`)

Drives the inference pool at max rate with synthetic frames (not capped by the 30fps camera):
```bash
cd firmware
YOLO_NPU_INSTANCES=1 YOLO_BENCH=300 ./build/bin/app   # single core
YOLO_NPU_INSTANCES=3 YOLO_BENCH=600 ./build/bin/app   # three cores
```

Measured (RK3588S):

| Model | 1 instance (1 core) | 3 instances (3 cores) | Speedup |
|---|---|---|---|
| yolov8n | ~37–43 fps | ~122–126 fps | ~3× |
| yolov8s | ~28 fps | ~70–78 fps | ~2.7× |

> Numbers vary with the NPU governor; run `sudo scripts/perf_setup.sh` first for stability.

## Video FPS (real pipeline, 1080p@30)

| Config | Video stream | Notes |
|---|---|---|
| yolov8n single-core (old, coupled) | 30fps | just keeps up |
| yolov8s single-core (old, coupled) | ~25.5fps ⬇ | inference stalls the video |
| yolov8s 3-core (new, decoupled) | 30fps ✓ | full FPS, zero detection drops |

Key wins: **decoupling** keeps video at a steady 30fps; the **3-core pool** sustains heavier models at full rate.

## Confirming 3-core utilization
```bash
watch -n1 cat /sys/kernel/debug/rknpu/load   # needs root
# e.g. Core0 62%  Core1 60%  Core2 61%
```

## Knobs
- `YOLO_NPU_INSTANCES` — instances/workers (3 for triple-core).
- `YOLO_DETECT_STRIDE` — detect every N frames; set 2 to offload heavy models.
- `scripts/perf_setup.sh` — clock locking.
- Model choice: yolov8n (fast) / yolov8s (heavier, more accurate).
