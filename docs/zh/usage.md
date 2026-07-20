# 使用说明

## 启动板端
```bash
cd firmware
CAMERA_EXCLUDE=/dev/video12 ./build/bin/app     # video14(iqtool) 已默认排除
```
启动日志会打印三核绑定、`[ZLM] rtc.externIP=...`、RTSP/WebRTC 地址、控制接口清单。按 `Ctrl+C` 退出。

放后台常驻：`nohup ./build/bin/app > app.log 2>&1 &`。
重启：`pkill -9 -x app` 然后重跑（**不要**用 `pkill -f build/bin/app`，会误杀你的 shell）。

### 环境变量

| 变量 | 作用 | 默认 |
|---|---|---|
| `YOLO_MODEL_PATH` | 模型路径 | `./model/yolov8n.rknn` |
| `YOLO_LABEL_PATH` | 标签路径 | `./model/coco_80_labels_list.txt` |
| `YOLO_NPU_INSTANCES` | NPU 实例数(1-3，绑三核) | `3` |
| `YOLO_DETECT_STRIDE` | 每 N 帧检测一次 | `1` |
| `YOLO_BENCH` | 压测模式：跑 N 次推理后退出 | 关闭 |
| `CAMERA_DEVICE` | 初始摄像头节点 | `/dev/video11` |
| `CAMERA_EXCLUDE` | 从可切换列表排除的节点(逗号分隔)；iqtool 已默认排除 | 空 |
| `RTC_EXTERN_IP` | WebRTC ICE 候选 IP(浏览器要能访问到) | 自动探测 |

## 启动网页平台
```bash
cd platform
CAM_BOARD_HOST=<BOARD_IP> npm start      # 或编辑 config.json
```
前提：板端已在运行。浏览器打开 **`http://<平台主机IP>:8090`**（不是板子的 8000）。
端口被占（`EADDRINUSE`）：`pkill -9 -x node` 或换端口 `CAM_LISTEN_PORT=8091 npm start`。

## 网页操作
- **开始监控**：拉起 WebRTC 实时画面（画面内含烧录的检测框）。
- **切换摄像头**：下拉选设备→切换（约 2~3 秒重连）；不出帧的节点约 10 秒后自愈回退。
- **开关识别**：一键开/关 YOLO。
- **状态面板**：视频/检测帧率、NPU 实例数、在推理数、分辨率、掉帧、模型、当前设备。

## 开机自启（systemd）
把下面路径中的 `$HOME` 换成实际用户主目录。板端 `/etc/systemd/system/edgevision.service`：
```ini
[Unit]
Description=EdgeVision-RK3588 firmware
After=network-online.target
[Service]
Type=simple
User=<你的用户名>
WorkingDirectory=$HOME/EdgeVision-RK3588/firmware
ExecStartPre=$HOME/EdgeVision-RK3588/firmware/scripts/perf_setup.sh
Environment=YOLO_NPU_INSTANCES=3
Environment=CAMERA_EXCLUDE=/dev/video12
ExecStart=$HOME/EdgeVision-RK3588/firmware/build/bin/app
Restart=on-failure
RestartSec=3
[Install]
WantedBy=multi-user.target
```
平台 `/etc/systemd/system/edgevision-web.service`：
```ini
[Unit]
Description=EdgeVision web platform
After=edgevision.service
[Service]
Type=simple
User=<你的用户名>
WorkingDirectory=$HOME/EdgeVision-RK3588/platform
Environment=CAM_BOARD_HOST=127.0.0.1
Environment=CAM_LISTEN_PORT=8090
ExecStart=/usr/bin/node server/server.js
Restart=on-failure
RestartSec=3
[Install]
WantedBy=multi-user.target
```
启用：`sudo systemctl daemon-reload && sudo systemctl enable --now edgevision edgevision-web`。
