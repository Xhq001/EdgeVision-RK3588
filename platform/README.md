# EdgeVision-RK3588 · platform（网页监控平台）

一个连接 RK3588 开发板的网页监控平台：**实时监控（WebRTC）+ 切换摄像头 + 开关智能识别（YOLO）+ 运行状态面板**。
纯 Node 内置模块，零第三方依赖。

配套板端程序：[`../firmware`](../firmware)（提供推流与 `/api/*` 控制接口）。完整文档见 [`../docs`](../docs)。

## 架构

```
浏览器  ──(同源)──►  platform 后端(Node)  ──反向代理──►  开发板 :8000
   │  · 静态页面 web/                              · /api/*      控制接口
   │  · /board/*  → 代理到板子                      · /index/api/webrtc  信令
   └──────────────  WebRTC 媒体(ICE 直连) ◄────────  开发板 RTC :8001
```

- 后端**纯 Node 内置模块，零第三方依赖**：静态托管前端 + 把 `/board/*` 反代到开发板，前端全部同源请求，规避跨域/混合内容。
- WebRTC **信令**走后端代理，**媒体流**由开发板经 ICE 直连浏览器（同一局域网内可达）。

## 运行

```bash
cd platform
# 1) 配置开发板地址（编辑 config.json，或用环境变量覆盖）
#    { "listenPort": 8090, "board": { "host": "<BOARD_IP>", "port": 8000 } }

# 2) 启动（需要板端 app 已在运行）
npm start
# 或： CAM_BOARD_HOST=<BOARD_IP> CAM_LISTEN_PORT=8090 node server/server.js

# 3) 浏览器打开
#    http://localhost:8090
```

环境变量覆盖：`CAM_BOARD_HOST` / `CAM_BOARD_PORT` / `CAM_LISTEN_PORT`。

## 功能

| 功能 | 说明 | 板端接口 |
|---|---|---|
| 实时监控 | WebRTC 低延迟拉流，画面内含烧录的检测框 | `POST /index/api/webrtc` |
| 切换摄像头 | 下拉选择自动枚举到的采集设备并热切换 | `GET /api/cameras` · `POST /api/camera` |
| 开关识别 | 一键开/关 YOLO 目标检测 | `POST /api/yolo` |
| 状态面板 | 视频/检测帧率、NPU 实例数、在推理数、分辨率、掉帧、模型 | `GET /api/status` |

## 说明

- 页面用 `<video>` + `RTCPeerConnection` 原生播放，无需插件；切换摄像头后约 2~3 秒自动重连画面。
- 若某个采集节点无法出图（如本项目板上的 ISP selfpath），板端会在约 10 秒后自动回退到上一个可用设备。
- 后端也可直接部署到开发板本机（node 已随镜像提供），则 `board.host` 填 `127.0.0.1`。
