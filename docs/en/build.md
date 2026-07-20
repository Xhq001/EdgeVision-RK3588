# Build & Prepare

## Hardware
- Rockchip RK3588 / RK3588S board
- Camera (MIPI or USB; USB should support MJPEG)

## System dependencies (once)
```bash
sudo apt update
sudo apt install -y cmake g++ make pkg-config libv4l-dev librockchip-mpp-dev librga-dev
# Install RKNN Runtime (librknnrt.so + rknn_api.h) per your board image.
# The platform backend needs Node (v18+).
node -v
```

## Build the firmware
```bash
cd firmware
cmake -S . -B build
cmake --build build -j$(nproc)
# Output: firmware/build/bin/app
```

## Prepare models
Put into `firmware/model/`:
- `yolov8n.rknn` (default, COCO 80 classes; yolov8s/m also work)
- `coco_80_labels_list.txt` (class names)

Models are large and not committed — see [firmware/model/README.md](../../firmware/model/README.md).
You can also point to absolute paths via `YOLO_MODEL_PATH` / `YOLO_LABEL_PATH`.

## Lock clocks (recommended for stable 3-core saturation)
```bash
sudo scripts/perf_setup.sh          # performance governor (NPU@1GHz + CPU big cores)
sudo scripts/perf_setup.sh restore  # restore defaults
```

## Code style
A [firmware/.clang-format](../../firmware/.clang-format) is provided; format changed files before committing:
```bash
clang-format -i firmware/src/*.cpp firmware/include/*.h
```
