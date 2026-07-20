# API Reference

The board exposes control endpoints on the embedded HTTP server (port 8000), all CORS-enabled.
Replace `<BOARD_IP>` with the board's actual IP.

## Control endpoints `/api/*`

| Method | Path | Body | Description |
|---|---|---|---|
| GET | `/api/status` | — | runtime status |
| GET | `/api/cameras` | — | available capture devices |
| POST | `/api/camera` | `{"device":"/dev/videoX"}` | hot-switch camera |
| POST | `/api/yolo` | `{"enable":true｜false}` | toggle detection |

```bash
B=http://<BOARD_IP>:8000
curl -s $B/api/status
curl -s $B/api/cameras
curl -s -X POST -d '{"device":"/dev/video12"}' $B/api/camera
curl -s -X POST -d '{"enable":false}' $B/api/yolo
```

`/api/status` example:
```json
{"code":0,"device":"/dev/video11","yolo_enabled":true,"yolo_available":true,
 "model":"./model/yolov8n.rknn","width":1920,"height":1080,"video_fps":29.9,
 "npu_instances":3,"detect_fps":30.0,"infer_ok":300,"infer_fail":0,
 "submit_dropped":0,"inflight":0}
```

## WebRTC play `/index/api/webrtc`

WHEP-style: the browser POSTs its SDP offer and receives JSON `{"code":0,"type":"answer","sdp":"..."}`.
```
POST http://<BOARD_IP>:8000/index/api/webrtc?app=live&stream=camera&type=play
Content-Type: application/sdp
<body: browser offer SDP>
```
Media flows over ICE directly to the board on 8001 (UDP+TCP); `externIP` is auto-set to the default-route
NIC IP (override with `RTC_EXTERN_IP`).

## RTSP
```
rtsp://<BOARD_IP>:8554/live/camera
```

## Via the platform proxy
The backend reverse-proxies `/board/*` to the board, so the frontend calls same-origin, e.g.
`GET /board/api/status`, `POST /board/index/api/webrtc?...`.
