# Models

`.rknn` 模型体积较大，**不纳入版本库**（见根 `.gitignore`）。运行前请把以下文件放到本目录：

Models are large and **not committed**. Before running, place these files here:

```
firmware/model/
├── yolov8n.rknn                 # 默认 / default (COCO 80-class)
└── coco_80_labels_list.txt      # 类别名 / class names
```

也可用更重的 `yolov8s.rknn` / `yolov8m.rknn`（更准、单核更慢，正好体现三核优势）。

## 获取方式 / How to obtain

- 用 Rockchip 官方 **rknn_model_zoo** 的 YOLOv8 例子导出/转换（需在 x86 主机用 rknn-toolkit2 把 `.onnx` 转成 `.rknn`）：
  https://github.com/airockchip/rknn_model_zoo
- `coco_80_labels_list.txt` 为标准 COCO 80 类名单（每行一个类别）。

## 指定路径 / Override paths

不放本目录也可用环境变量指定绝对路径：
```bash
YOLO_MODEL_PATH=/abs/path/yolov8n.rknn YOLO_LABEL_PATH=/abs/path/coco_80_labels_list.txt ./build/bin/app
```
