# Troubleshooting

## WebRTC shows OFFLINE / "connection failed" (signaling OK but no video)
Most likely first:

1. **Firewall blocking the media port**: WebRTC media uses `8001` (UDP+TCP). If the board runs ufw:
   ```bash
   sudo ufw allow 8001 && sudo ufw allow 8000 && sudo ufw allow 8554 && sudo ufw allow 8090
   ```
2. **H.264 profile**: browser WebRTC only supports Constrained Baseline; this project is fixed to Baseline.
   Verify: `ffprobe -rtsp_transport tcp rtsp://<BOARD_IP>:8554/live/camera` should show
   `profile=Constrained Baseline`. If it shows High/Main you are running an old binary — rebuild and
   `pkill -9 -x app` to restart.
3. **ICE candidate IP**: with multiple NICs (incl. docker `172.17.x`) you need the correct `externIP`.
   Startup log should show `[ZLM] rtc.externIP=...`; override with
   `RTC_EXTERN_IP=<board-ip-reachable-by-browser> ./build/bin/app`.
4. **Reachability**: WebRTC media is a direct browser↔board LAN connection; it won't traverse different
   subnets / cellular. Fall back to RTSP if needed.

Board-local self-test: `pip install aiortc`, then a small WHEP client against `127.0.0.1:8000`. If it
receives frames, the board is fine and the issue is the network/firewall.

## External device shows ZLMediaKit's "resource not found"
You opened the board's **8000** (media/API port), not the web UI. Open the **platform on 8090**:
`http://<platform-host>:8090`. (This project adds a friendly hint page at the board's `/`.)

## Platform: `EADDRINUSE :::8090`
A previous platform instance holds the port: `pkill -9 -x node`, or `CAM_LISTEN_PORT=8091 npm start`.

## Platform: "cannot connect to board ...:8000 ECONNREFUSED"
The firmware isn't running, or `config.json` has the wrong board IP. Start the firmware first;
`curl http://<BOARD_IP>:8000/api/status` should return JSON.

## Switching camera → black for a few seconds then reverts / app exits
- A non-producing node (e.g. this board's selfpath/iqtool) triggers a ~10s timeout, then **auto-reverts** to
  the last working camera — it does not exit.
- Use `CAMERA_EXCLUDE=/dev/video12` to hide non-producing nodes (video14/iqtool excluded by default).
- On a transient camera stall the pipeline restarts the same device and retries; it only gives up after 5
  consecutive rapid failures.

## No detection boxes in the frame
- Check `[YOLO] enabled` in logs and `yolo_enabled:true` in `/api/status`; the frame must contain a
  COCO-detectable object (person/car/bottle...).

## Can't kill the process
The app handles `Ctrl+C` (SIGINT); ZLM intercepts SIGTERM. Kill it with `pkill -9 -x app`
(**not** `pkill -f build/bin/app`, which matches your own shell and kills your terminal).
