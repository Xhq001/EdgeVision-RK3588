# 接口说明

板端在内嵌 HTTP 服务（默认 8000 端口）上提供控制接口，全部带 CORS。`<BOARD_IP>` 替换为开发板实际 IP。

## 控制接口 `/api/*`

| 方法 | 路径 | 请求体 | 说明 |
|---|---|---|---|
| GET | `/api/status` | — | 运行状态 |
| GET | `/api/cameras` | — | 可用采集设备列表 |
| POST | `/api/camera` | `{"device":"/dev/videoX"}` | 热切换摄像头 |
| POST | `/api/yolo` | `{"enable":true｜false}` | 开关识别 |

示例：
```bash
B=http://<BOARD_IP>:8000
curl -s $B/api/status
curl -s $B/api/cameras
curl -s -X POST -d '{"device":"/dev/video12"}' $B/api/camera
curl -s -X POST -d '{"enable":false}' $B/api/yolo
```

`/api/status` 返回示例：
```json
{"code":0,"device":"/dev/video11","yolo_enabled":true,"yolo_available":true,
 "model":"./model/yolov8n.rknn","width":1920,"height":1080,"video_fps":29.9,
 "npu_instances":3,"detect_fps":30.0,"infer_ok":300,"infer_fail":0,
 "submit_dropped":0,"inflight":0}
```

## WebRTC 播放协商 `/index/api/webrtc`

WHEP 风格：浏览器 POST 自己的 SDP offer，返回 JSON `{"code":0,"type":"answer","sdp":"..."}`。
```
POST http://<BOARD_IP>:8000/index/api/webrtc?app=live&stream=camera&type=play
Content-Type: application/sdp
<body: 浏览器 offer SDP>
```
媒体走 ICE 直连开发板 8001（UDP+TCP）；`externIP` 会自动设为默认路由网卡 IP（可用 `RTC_EXTERN_IP` 覆盖）。

## RTSP

```
rtsp://<BOARD_IP>:8554/live/camera
```

## 经平台代理

平台后端把 `/board/*` 反代到开发板，前端同源调用，例如：
`GET /board/api/status`、`POST /board/index/api/webrtc?...`。
