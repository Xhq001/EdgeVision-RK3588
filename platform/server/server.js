'use strict';

// RK3588 监控平台轻量后端（纯 Node 内置模块，零第三方依赖）：
//   1) 静态托管 web/ 前端；
//   2) 把 /board/* 反向代理到开发板 http://<board.host>:<board.port>/*
//      （包含 /api/* 控制接口与 /index/api/webrtc WebRTC 信令），
//      使前端全部走同源请求，规避跨域/混合内容问题。
//   WebRTC 媒体流仍由开发板经 ICE 直连浏览器（局域网内正常）。

const http = require('http');
const fs = require('fs');
const path = require('path');
const url = require('url');

// ---- 读取配置（config.json，可被环境变量覆盖）----
const CONFIG_PATH = path.join(__dirname, '..', 'config.json');
let cfg = { listenPort: 8090, board: { host: '192.168.1.100', port: 8000 } };
try {
  cfg = Object.assign(cfg, JSON.parse(fs.readFileSync(CONFIG_PATH, 'utf8')));
} catch (e) {
  console.warn('[cam-platform] 未能读取 config.json，使用默认配置:', e.message);
}
const LISTEN_PORT = parseInt(process.env.CAM_LISTEN_PORT || cfg.listenPort, 10) || 8090;
const BOARD_HOST = process.env.CAM_BOARD_HOST || cfg.board.host;
const BOARD_PORT = parseInt(process.env.CAM_BOARD_PORT || cfg.board.port, 10) || 8000;

const WEB_DIR = path.join(__dirname, '..', 'web');
const MIME = {
  '.html': 'text/html; charset=utf-8',
  '.js': 'application/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8',
  '.json': 'application/json; charset=utf-8',
  '.svg': 'image/svg+xml',
  '.ico': 'image/x-icon',
  '.png': 'image/png'
};

function sendJson(res, code, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(code, { 'Content-Type': 'application/json; charset=utf-8' });
  res.end(body);
}

// 反向代理到开发板：把 /board/<rest> 转发为 http://BOARD_HOST:BOARD_PORT/<rest>
function proxyToBoard(req, res, boardPath) {
  const chunks = [];
  req.on('data', (c) => chunks.push(c));
  req.on('end', () => {
    const body = Buffer.concat(chunks);
    const options = {
      host: BOARD_HOST,
      port: BOARD_PORT,
      method: req.method,
      path: boardPath,
      headers: {
        Host: `${BOARD_HOST}:${BOARD_PORT}`,
        'Content-Type': req.headers['content-type'] || 'application/json',
        'Content-Length': body.length
      },
      timeout: 15000
    };
    const preq = http.request(options, (pres) => {
      const pchunks = [];
      pres.on('data', (c) => pchunks.push(c));
      pres.on('end', () => {
        const pbody = Buffer.concat(pchunks);
        res.writeHead(pres.statusCode || 502, {
          'Content-Type': pres.headers['content-type'] || 'application/json; charset=utf-8'
        });
        res.end(pbody);
      });
    });
    preq.on('timeout', () => preq.destroy(new Error('board timeout')));
    preq.on('error', (err) => {
      sendJson(res, 502, { code: -1, msg: `无法连接开发板 ${BOARD_HOST}:${BOARD_PORT}: ${err.message}` });
    });
    if (body.length) preq.write(body);
    preq.end();
  });
}

function serveStatic(req, res, pathname) {
  let rel = pathname === '/' ? '/index.html' : pathname;
  // 防目录穿越
  const safe = path.normalize(rel).replace(/^(\.\.[\/\\])+/, '');
  const filePath = path.join(WEB_DIR, safe);
  if (!filePath.startsWith(WEB_DIR)) {
    res.writeHead(403);
    res.end('forbidden');
    return;
  }
  fs.readFile(filePath, (err, data) => {
    if (err) {
      res.writeHead(404, { 'Content-Type': 'text/plain; charset=utf-8' });
      res.end('404 Not Found');
      return;
    }
    const ext = path.extname(filePath).toLowerCase();
    res.writeHead(200, { 'Content-Type': MIME[ext] || 'application/octet-stream' });
    res.end(data);
  });
}

const server = http.createServer((req, res) => {
  const parsed = url.parse(req.url);
  const pathname = parsed.pathname || '/';

  // 前端所需的板子地址（用于展示 / WebRTC 直连信息）
  if (pathname === '/config') {
    sendJson(res, 200, {
      board: { host: BOARD_HOST, port: BOARD_PORT },
      // WebRTC 信令走同源代理，媒体由板子 ICE 直连
      webrtcPath: '/board/index/api/webrtc',
      app: 'live',
      stream: 'camera'
    });
    return;
  }

  // 反向代理到开发板
  if (pathname.startsWith('/board/')) {
    const boardPath = pathname.slice('/board'.length) + (parsed.search || '');
    proxyToBoard(req, res, boardPath);
    return;
  }

  serveStatic(req, res, pathname);
});

server.listen(LISTEN_PORT, () => {
  console.log('==============================================');
  console.log(' RK3588 监控平台 EdgeVision');
  console.log(` 本地访问:   http://localhost:${LISTEN_PORT}`);
  console.log(` 代理开发板: http://${BOARD_HOST}:${BOARD_PORT}`);
  console.log(' 覆盖: CAM_BOARD_HOST / CAM_BOARD_PORT / CAM_LISTEN_PORT');
  console.log('==============================================');
});
