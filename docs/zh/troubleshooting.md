# 常见问题排查

## WebRTC 显示 OFFLINE / "连接失败"（信令通了但画面出不来）
按可能性从高到低：

1. **防火墙挡了媒体端口**：WebRTC 媒体走 `8001`（UDP+TCP）。开发板若开了 ufw：
   ```bash
   sudo ufw allow 8001 && sudo ufw allow 8000 && sudo ufw allow 8554 && sudo ufw allow 8090
   ```
2. **H.264 profile**：浏览器 WebRTC 只支持 Constrained Baseline，本项目已固定为 Baseline。
   验证：`ffprobe -rtsp_transport tcp rtsp://<BOARD_IP>:8554/live/camera` 应显示 `profile=Constrained Baseline`；
   若显示 High/Main，说明跑的是旧二进制，重新编译并 `pkill -9 -x app` 重启。
3. **ICE 候选 IP**：多网卡（含 docker `172.17.x`）时需正确的 `externIP`。启动日志应有 `[ZLM] rtc.externIP=...`；
   不对就 `RTC_EXTERN_IP=<浏览器能访问的板子IP> ./build/bin/app`。
4. **网段可达性**：WebRTC 媒体是浏览器直连板子 LAN IP，跨网段/走手机流量连不上；可退回 RTSP。

板端本机自测（确认板子侧没问题）：装 `pip install aiortc` 后用一个 WHEP 客户端连 `127.0.0.1:8000`，
能收到帧即说明板子 OK，问题在网络/防火墙。

## 外部设备打开显示"您访问的资源不存在！"
那是 ZLMediaKit 的 404——你开的是板子 **8000**（媒体/接口端口），不是网页。请开**平台的 8090**：
`http://<平台主机IP>:8090`。（本项目已给 8000 根路径加了友好提示页。）

## 平台报 `EADDRINUSE :::8090`
端口被上一个平台实例占了：`pkill -9 -x node`，或换端口 `CAM_LISTEN_PORT=8091 npm start`。

## 平台报"无法连接开发板 ...:8000 ECONNREFUSED"
板端 `app` 没在跑，或 `config.json` 里板子 IP 不对。先把板端跑起来，
`curl http://<BOARD_IP>:8000/api/status` 能返回 JSON 即正常。

## 切换摄像头后黑屏几秒又切回去 / 程序退出
- 目标节点不出帧（如本板 selfpath/iqtool），约 10 秒超时后**自动回退**到上一个可用摄像头，不会退出。
- 用 `CAMERA_EXCLUDE=/dev/video12` 把不出帧的节点从下拉列表去掉（video14/iqtool 已默认排除）。
- 相机偶发卡顿时程序会自动重启该路重试，连续 5 次快速失败才放弃。

## 画面里没有检测框
- 看日志有 `[YOLO] enabled`；`/api/status` 里 `yolo_enabled:true`；画面里要有 COCO 能识别的目标（人/车/瓶子等）。

## 进程杀不掉
程序只处理 `Ctrl+C`(SIGINT)，ZLM 会拦 SIGTERM；后台收尾用 `pkill -9 -x app`
（**别**用 `pkill -f build/bin/app`，那会匹配到你的 shell 命令行、把终端一起杀掉）。
