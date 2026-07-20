'use strict';

// ---- 全局状态 ----
let CONFIG = { webrtcPath: '/board/index/api/webrtc', app: 'live', stream: 'camera',
               board: { host: '', port: 0 } };
let pc = null;
let playing = false;
let statusTimer = null;
let userToggling = false; // 避免状态轮询把用户刚点的开关又刷回去

const $ = (id) => document.getElementById(id);
const els = {};
['video', 'videoOverlay', 'playBtn', 'stopBtn', 'videoState', 'liveBadge',
 'cameraSelect', 'switchBtn', 'refreshCamBtn', 'cameraHint',
 'yoloToggle', 'yoloHint', 'connDot', 'connText', 'boardAddr',
 'stVideoFps', 'stDetectFps', 'stNpu', 'stInflight', 'stRes', 'stDropped',
 'stModel', 'stDevice', 'stInferOk', 'toast'].forEach((id) => (els[id] = $(id)));

function toast(msg, isErr) {
  els.toast.textContent = msg;
  els.toast.className = 'toast show' + (isErr ? ' err' : '');
  clearTimeout(toast._t);
  toast._t = setTimeout(() => (els.toast.className = 'toast'), 2600);
}

async function api(path, opts) {
  const resp = await fetch(path, opts);
  const text = await resp.text();
  try { return JSON.parse(text); } catch (e) { throw new Error('无效响应: ' + text.slice(0, 120)); }
}

// ---- 配置 ----
async function loadConfig() {
  try {
    const c = await api('/config');
    CONFIG = Object.assign(CONFIG, c);
    els.boardAddr.textContent = `${c.board.host}:${c.board.port}`;
  } catch (e) {
    els.boardAddr.textContent = '(读取配置失败)';
  }
}

// ---- WebRTC 拉流（WHEP 风格，经后端同源代理到开发板）----
function webrtcUrl() {
  return `${CONFIG.webrtcPath}?app=${CONFIG.app}&stream=${CONFIG.stream}&type=play`;
}

function closePc() {
  if (pc) {
    pc.ontrack = null;
    pc.onconnectionstatechange = null;
    try { pc.close(); } catch (e) {}
    pc = null;
  }
  els.video.srcObject = null;
}

async function waitIce(peer) {
  if (peer.iceGatheringState === 'complete') return;
  await new Promise((resolve) => {
    const t = setTimeout(resolve, 1500); // 兜底：ICE 收集最多等 1.5s
    peer.addEventListener('icegatheringstatechange', function onS() {
      if (peer.iceGatheringState === 'complete') {
        clearTimeout(t);
        peer.removeEventListener('icegatheringstatechange', onS);
        resolve();
      }
    });
  });
}

async function startPlay(silent) {
  closePc();
  setLive(false, '连接中…');
  try {
    pc = new RTCPeerConnection({ iceServers: [] });
    pc.addTransceiver('video', { direction: 'recvonly' });
    pc.ontrack = (e) => {
      if (e.streams && e.streams[0]) {
        els.video.srcObject = e.streams[0];
        els.videoOverlay.classList.add('hidden');
      }
    };
    pc.onconnectionstatechange = () => {
      const s = pc ? pc.connectionState : 'closed';
      if (s === 'connected') { playing = true; setLive(true, '监控中'); }
      else if (s === 'failed' || s === 'disconnected' || s === 'closed') {
        if (playing) setLive(false, s === 'failed' ? '连接失败' : '已断开');
      }
    };

    const offer = await pc.createOffer({ offerToReceiveVideo: true, offerToReceiveAudio: false });
    await pc.setLocalDescription(offer);
    await waitIce(pc);

    const resp = await fetch(webrtcUrl(), {
      method: 'POST',
      headers: { 'Content-Type': 'application/sdp' },
      body: pc.localDescription.sdp
    });
    const data = await resp.json();
    if (data.code !== 0 || !data.sdp) throw new Error(data.msg || JSON.stringify(data));
    await pc.setRemoteDescription({ type: 'answer', sdp: data.sdp });
    playing = true;
    els.playBtn.textContent = '重新连接';
  } catch (err) {
    setLive(false, '失败');
    closePc();
    if (!silent) toast('拉流失败: ' + (err.message || err), true);
  }
}

function stopPlay() {
  playing = false;
  closePc();
  els.videoOverlay.classList.remove('hidden');
  els.playBtn.textContent = '开始监控';
  setLive(false, '已停止');
}

function setLive(on, stateText) {
  els.liveBadge.className = 'live-badge ' + (on ? 'on' : 'off');
  els.liveBadge.textContent = on ? '● LIVE' : '● OFFLINE';
  if (stateText) els.videoState.textContent = stateText;
}

// ---- 摄像头列表 / 切换 ----
async function refreshCameras() {
  try {
    const data = await api('/board/api/cameras');
    if (data.code !== 0) throw new Error(data.msg || 'cameras error');
    const cur = data.current || '';
    els.cameraSelect.innerHTML = '';
    (data.cameras || []).forEach((c) => {
      const opt = document.createElement('option');
      opt.value = c.device;
      opt.textContent = `${c.card} (${c.device})`;
      if (c.device === cur) opt.selected = true;
      els.cameraSelect.appendChild(opt);
    });
    const sel = (data.cameras || []).find((c) => c.device === cur);
    els.cameraHint.textContent = sel ? `格式: ${sel.formats}` : `共 ${(data.cameras || []).length} 个可用设备`;
  } catch (e) {
    els.cameraHint.textContent = '设备列表获取失败';
  }
}

async function switchCamera() {
  const dev = els.cameraSelect.value;
  if (!dev) return;
  els.switchBtn.disabled = true;
  els.switchBtn.textContent = '切换中…';
  try {
    const data = await api('/board/api/camera', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ device: dev })
    });
    if (data.code !== 0) throw new Error(data.msg || '切换失败');
    toast('已切换到 ' + dev + '，正在重连画面…');
    // 板端流水线重启需要 1~2 秒，稍后自动重连视频
    setTimeout(() => { if (playing || els.video.srcObject === null) startPlay(true); }, 2500);
  } catch (e) {
    toast('切换失败: ' + (e.message || e), true);
  } finally {
    els.switchBtn.disabled = false;
    els.switchBtn.textContent = '切换';
  }
}

// ---- YOLO 开关 ----
async function toggleYolo() {
  const enable = els.yoloToggle.checked;
  userToggling = true;
  try {
    const data = await api('/board/api/yolo', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ enable })
    });
    if (data.code !== 0) throw new Error(data.msg || 'yolo error');
    toast('智能识别已' + (enable ? '开启' : '关闭'));
  } catch (e) {
    els.yoloToggle.checked = !enable; // 回滚
    toast('操作失败: ' + (e.message || e), true);
  } finally {
    setTimeout(() => (userToggling = false), 800);
  }
}

// ---- 状态轮询 ----
function setConn(state) {
  // state: 'on' | 'off' | 'wait'
  els.connDot.className = 'dot dot-' + state;
  els.connText.textContent = state === 'on' ? '已连接' : state === 'wait' ? '连接中' : '未连接';
}

async function pollStatus() {
  try {
    const s = await api('/board/api/status');
    if (s.code !== 0) throw new Error('status error');
    setConn('on');
    els.stVideoFps.textContent = fmt(s.video_fps, 1);
    els.stDetectFps.textContent = s.yolo_enabled ? fmt(s.detect_fps, 1) : '关';
    els.stNpu.textContent = s.npu_instances != null ? s.npu_instances : '–';
    els.stInflight.textContent = s.inflight != null ? s.inflight : '–';
    els.stRes.textContent = (s.width && s.height) ? `${s.width}×${s.height}` : '–';
    els.stDropped.textContent = s.dropped_frames != null ? s.dropped_frames : '–';
    els.stModel.textContent = shortModel(s.model);
    els.stDevice.textContent = s.device || '–';
    els.stInferOk.textContent = s.infer_ok != null ? s.infer_ok : '–';
    // 同步 YOLO 开关（避免打断用户刚点的操作）
    if (!userToggling) {
      els.yoloToggle.checked = !!s.yolo_enabled;
      els.yoloToggle.disabled = !s.yolo_available;
      els.yoloHint.textContent = s.yolo_available
        ? (s.yolo_enabled ? '检测中 · 框已烧录进画面' : '已关闭')
        : '模型未加载';
    }
  } catch (e) {
    setConn('off');
    ['stVideoFps', 'stDetectFps', 'stNpu', 'stInflight', 'stRes', 'stDropped'].forEach(
      (k) => (els[k].textContent = '–')
    );
  }
}

function fmt(v, d) { return (v == null || isNaN(v)) ? '–' : Number(v).toFixed(d); }
function shortModel(m) { if (!m) return '–'; const p = m.split('/'); return p[p.length - 1]; }

// ---- 事件绑定 ----
els.playBtn.onclick = () => startPlay(false);
els.stopBtn.onclick = stopPlay;
els.videoOverlay.onclick = () => startPlay(false);
els.switchBtn.onclick = switchCamera;
els.refreshCamBtn.onclick = refreshCameras;
els.yoloToggle.onchange = toggleYolo;

// ---- 启动 ----
(async function init() {
  setConn('wait');
  await loadConfig();
  await refreshCameras();
  await pollStatus();
  statusTimer = setInterval(pollStatus, 1500);
})();
