# Third-Party Notices

This project builds on the following third-party components. Their respective licenses apply.

## Prebuilt runtime libraries (in `firmware/lib/`)

- `libmk_api.so`, `libZLToolKit.so` — from **ZLMediaKit** (https://github.com/ZLMediaKit/ZLMediaKit), MIT License.
  Provided for convenience on RK3588 targets. If your platform/ABI differs, rebuild/replace them.

## System libraries (expected on the RK3588 board image)

- **Rockchip MPP** (`librockchip_mpp`) — hardware video encode/decode.
- **Rockchip RGA** (`librga`) — 2D image processing (CSC/scale/copy).
- **RKNN Runtime** (`librknnrt.so`, `rknn_api.h`) — NPU inference runtime.
- **libv4l2** — V4L2 capture.

These are part of the Rockchip BSP / board image and are not redistributed here.

## Models & references

- YOLOv8 / YOLOv10 `.rknn` models are **not** included (see `firmware/model/README.md` for how to obtain them).
  YOLO models originate from Ultralytics; RKNN conversions and detection post-processing follow
  **rknn-toolkit2 / rknn_model_zoo** (https://github.com/airockchip/rknn-toolkit2).
- The multi-instance NPU pattern (`rknn_dup_context` + per-core `rknn_set_core_mask`) follows the
  Rockchip community `rknn_pool` approach.

## Upstream

- The video pipeline design references **JoeChen2me/RK-MediaProject**
  (https://github.com/JoeChen2me/RK-MediaProject).
