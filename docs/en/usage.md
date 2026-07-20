# Usage

## Run the firmware
```bash
cd firmware
CAMERA_EXCLUDE=/dev/video12 ./build/bin/app     # video14(iqtool) is excluded by default
```
Startup logs print the 3-core binding, `[ZLM] rtc.externIP=...`, RTSP/WebRTC URLs and the control
endpoints. Press `Ctrl+C` to quit.

Run detached: `nohup ./build/bin/app > app.log 2>&1 &`.
Restart: `pkill -9 -x app` then rerun. **Do not** use `pkill -f build/bin/app` — it matches your own
shell command line and kills your terminal.

### Environment variables

| Variable | Purpose | Default |
|---|---|---|
| `YOLO_MODEL_PATH` | model path | `./model/yolov8n.rknn` |
| `YOLO_LABEL_PATH` | labels path | `./model/coco_80_labels_list.txt` |
| `YOLO_NPU_INSTANCES` | NPU instances (1-3, pinned to cores) | `3` |
| `YOLO_DETECT_STRIDE` | detect every N frames | `1` |
| `YOLO_BENCH` | benchmark mode: run N inferences then exit | off |
| `CAMERA_DEVICE` | initial camera node | `/dev/video11` |
| `CAMERA_EXCLUDE` | nodes to exclude from the list (comma-separated); iqtool excluded by default | empty |
| `RTC_EXTERN_IP` | WebRTC ICE candidate IP (must be reachable by the browser) | auto-detect |

## Run the web platform
```bash
cd platform
CAM_BOARD_HOST=<BOARD_IP> npm start      # or edit config.json
```
Requires the firmware running. Open **`http://<platform-host>:8090`** (not the board's 8000).
Port in use (`EADDRINUSE`): `pkill -9 -x node`, or `CAM_LISTEN_PORT=8091 npm start`.

## Web UI
- **Start monitoring** — WebRTC live view (detection boxes burned into the frame).
- **Switch camera** — pick a device → switch (~2–3s reconnect); non-producing nodes self-heal back after ~10s.
- **Toggle detection** — YOLO on/off.
- **Status panel** — video/detect FPS, NPU instances, in-flight, resolution, drops, model, current device.

## systemd auto-start
Replace `$HOME` with the actual home dir. Firmware `/etc/systemd/system/edgevision.service`:
```ini
[Unit]
Description=EdgeVision-RK3588 firmware
After=network-online.target
[Service]
Type=simple
User=<your-user>
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
Platform `/etc/systemd/system/edgevision-web.service`:
```ini
[Unit]
Description=EdgeVision web platform
After=edgevision.service
[Service]
Type=simple
User=<your-user>
WorkingDirectory=$HOME/EdgeVision-RK3588/platform
Environment=CAM_BOARD_HOST=127.0.0.1
Environment=CAM_LISTEN_PORT=8090
ExecStart=/usr/bin/node server/server.js
Restart=on-failure
RestartSec=3
[Install]
WantedBy=multi-user.target
```
Enable: `sudo systemctl daemon-reload && sudo systemctl enable --now edgevision edgevision-web`.
