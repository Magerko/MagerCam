// Притворяюсь телефоном MagerCam, чтобы проверять плагин OBS без настоящего телефона.
// Тестовую картинку и тон 440 Гц генерирую через ffmpeg и отдаю по протоколу docs/PROTOCOL.md.
// Запуск: node tools/fake-phone.js [port]
'use strict';
const net = require('net');
const dgram = require('dgram');
const { spawn } = require('child_process');
const os = require('os');

const PORT = Number(process.argv[2] || 4848);
const state = {
  name: 'Fake Phone (' + os.hostname() + ')', ver: 1,
  w: 1280, h: 720, fps: 30, bitrate: 4000, camera: '0', front: false,
  cameras: [{ id: '0', front: false, label: 'Задняя (авто, все объективы)' },
    { id: '0:2', front: false, label: 'Задняя широкоугольная (13 мм)' },
    { id: '0:3', front: false, label: 'Задняя телефото (77 мм)' },
    { id: '1', front: true, label: 'Фронтальная (26 мм)' }],
  has_flash: true, zoom_min: 1,
  orient: 0, audio: true, torch: false, zoom: 1, zoom_max: 8, ev: 0, ev_min: -2, ev_max: 2,
  focus: 'continuous', wb: 'auto', antibanding: 'auto', stab: false,
  battery: 77, charging: false, temp: 30, clients: 0,
};
const t0 = process.hrtime.bigint();
const nowUs = () => Number((process.hrtime.bigint() - t0) / 1000n);

function packet(type, payload, pts, key) {
  const h = Buffer.alloc(16);
  h.write('MC', 0, 'ascii');
  h[2] = type; h[3] = key ? 1 : 0;
  h.writeUInt32BE(payload.length, 4);
  h.writeBigInt64BE(BigInt(pts), 8);
  return Buffer.concat([h, payload]);
}

function applyParams(q) {
  for (const [k, v] of q) {
    if (k === 'w') state.w = +v; else if (k === 'h') state.h = +v;
    else if (k === 'fps') state.fps = +v; else if (k === 'bitrate') state.bitrate = +v;
    else if (k === 'camera') {
      const c = state.cameras.find((x) => x.id === v);
      if (c) { state.camera = c.id; state.front = c.front; }
      else { state.front = v === 'front'; state.camera = state.front ? '1' : '0'; }
    }
    else if (k === 'audio') state.audio = v === '1';
    else if (k === 'torch') state.torch = v === 'toggle' ? !state.torch : v === '1';
    else if (k === 'zoom') state.zoom = +v; else if (k === 'ev') state.ev = +v;
    else if (k === 'orient') state.orient = +v;
    else if (k in state) state[k] = v;
  }
}

function* nals(buf) {
  let i = 0, start = -1;
  while (i + 3 <= buf.length) {
    if (buf[i] === 0 && buf[i + 1] === 0 && (buf[i + 2] === 1 || (buf[i + 2] === 0 && buf[i + 3] === 1))) {
      const sc = buf[i + 2] === 1 ? 3 : 4;
      if (start >= 0) yield buf.subarray(start, i);
      start = i; i += sc;
    } else i++;
  }
  if (start >= 0) yield buf.subarray(start);
}

function startStream(sock, query) {
  applyParams(query);
  state.clients++;
  console.log(`[stream] client ${sock.remoteAddress} ${state.w}x${state.h}@${state.fps} ${state.bitrate}kbps front=${state.front}`);
  sock.write('HTTP/1.1 200 OK\r\nContent-Type: application/x-magercam\r\n\r\n');
  const send = (b) => { if (!sock.destroyed) sock.write(b); };

  const label = (state.front ? 'FRONT' : 'BACK') + ' cam ' + state.camera.replace(':', '-');
  const vf = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-re', '-f', 'lavfi',
    '-i', `testsrc2=size=${state.w}x${state.h}:rate=${state.fps}`,
    '-vf', `drawtext=fontfile=arial.ttf:text='MagerCam ${label} %{localtime\\:%T}':fontsize=${Math.round(state.h / 12)}:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=h*0.1`,
    '-c:v', 'libx264', '-preset', 'ultrafast', '-tune', 'zerolatency', '-profile:v', 'baseline',
    '-b:v', `${state.bitrate}k`, '-g', String(state.fps), '-x264-params', 'aud=1:repeat-headers=1',
    '-f', 'h264', '-'], { cwd: process.env.WINDIR ? process.env.WINDIR + '/Fonts' : undefined });
  let vbuf = Buffer.alloc(0), au = [], sentConfig = false;
  const flushAU = () => {
    if (!au.length) return;
    const types = au.map((n) => n[n[2] === 1 ? 3 : 4] & 0x1f);
    if (!sentConfig && types.includes(7)) {
      const cfg = Buffer.concat(au.filter((n, i) => types[i] === 7 || types[i] === 8));
      send(packet(1, cfg, nowUs(), true));
      sentConfig = true;
    }
    if (sentConfig) send(packet(2, Buffer.concat(au), nowUs(), types.includes(5)));
    au = [];
  };
  vf.stdout.on('data', (d) => {
    vbuf = Buffer.concat([vbuf, d]);
    const units = [...nals(vbuf)];
    // последний NAL может быть недописан, поэтому держу его в буфере до следующего куска
    const last = units.pop();
    vbuf = last ? Buffer.from(last) : Buffer.alloc(0);
    for (const n of units) {
      const t = n[n[2] === 1 ? 3 : 4] & 0x1f;
      if (t === 9) flushAU();
      au.push(Buffer.from(n));
    }
  });
  vf.stderr.on('data', (d) => process.stderr.write('[ffmpeg-v] ' + d));

  const cfg = Buffer.alloc(5); cfg.writeUInt32BE(48000, 0); cfg[4] = 1;
  send(packet(3, cfg, nowUs(), false));
  const af = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-re', '-f', 'lavfi',
    '-i', 'sine=frequency=440:sample_rate=48000', '-ac', '1', '-f', 's16le', '-']);
  let abuf = Buffer.alloc(0);
  af.stdout.on('data', (d) => {
    abuf = Buffer.concat([abuf, d]);
    while (abuf.length >= 1920) { // 1920 байт — это 20 мс моно s16 на 48 кГц
      const chunk = abuf.subarray(0, 1920); abuf = abuf.subarray(1920);
      if (state.audio) send(packet(4, Buffer.from(chunk), nowUs() - 20000, false));
    }
  });

  const status = setInterval(() => send(packet(5, Buffer.from(JSON.stringify(state)), nowUs(), false)), 1000);
  send(packet(5, Buffer.from(JSON.stringify(state)), nowUs(), false));

  const cleanup = () => { clearInterval(status); vf.kill(); af.kill(); state.clients--; console.log('[stream] client gone'); };
  sock.on('close', cleanup);
  sock.on('error', () => {});
}

net.createServer((sock) => {
  let head = '';
  sock.setNoDelay(true);
  const onData = (d) => {
    head += d.toString('latin1');
    const end = head.indexOf('\r\n\r\n');
    if (end < 0) return;
    sock.removeListener('data', onData);
    const [method, target] = head.split(' ');
    const url = new URL(target || '/', 'http://x');
    console.log(`[http] ${method} ${url.pathname}${url.search}`);
    if (url.pathname === '/stream') return startStream(sock, url.searchParams);
    if (url.pathname === '/api/set') applyParams(url.searchParams);
    if (url.pathname.startsWith('/api/')) {
      const body = JSON.stringify(state);
      sock.end(`HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nContent-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`);
      return;
    }
    sock.end('HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n');
  };
  sock.on('data', onData);
  sock.on('error', () => {});
}).listen(PORT, () => console.log(`fake phone on tcp ${PORT}, discovery on udp 4849`));

const udp = dgram.createSocket({ type: 'udp4', reuseAddr: true });
udp.on('message', (msg, rinfo) => {
  if (msg.toString() !== 'MAGERCAM_DISCOVER') return;
  console.log(`[discovery] from ${rinfo.address}:${rinfo.port}`);
  udp.send(JSON.stringify({ magercam: 1, name: state.name, port: PORT }), rinfo.port, rinfo.address);
});
udp.bind(4849);
