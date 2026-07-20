# 编译与准备

## 硬件
- Rockchip RK3588 / RK3588S 开发板
- 摄像头（MIPI 或 USB，USB 建议支持 MJPEG）

## 系统依赖（一次性）
```bash
sudo apt update
sudo apt install -y cmake g++ make pkg-config libv4l-dev librockchip-mpp-dev librga-dev
# RKNN Runtime（librknnrt.so + rknn_api.h）按板卡镜像方式安装
# 平台后端需要 Node（v18+）
node -v
```

## 编译 firmware
```bash
cd firmware
cmake -S . -B build
cmake --build build -j$(nproc)
# 产物：firmware/build/bin/app
```

## 准备模型
`firmware/model/` 需要放入：
- `yolov8n.rknn`（默认，COCO 80 类；也可换 yolov8s/m）
- `coco_80_labels_list.txt`（类别名）

模型体积较大、不入库，获取方式见 [firmware/model/README.md](../../firmware/model/README.md)。
也可用环境变量指定绝对路径：`YOLO_MODEL_PATH` / `YOLO_LABEL_PATH`。

## 锁频（推荐，压满三核更稳）
```bash
sudo scripts/perf_setup.sh          # 锁 performance（NPU@1GHz + CPU 大核）
sudo scripts/perf_setup.sh restore  # 恢复默认
```

## 代码风格
仓库提供 [firmware/.clang-format](../../firmware/.clang-format)，提交前对改动文件格式化：
```bash
clang-format -i firmware/src/*.cpp firmware/include/*.h
```
