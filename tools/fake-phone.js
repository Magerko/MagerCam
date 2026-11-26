// Притворяюсь телефоном MagerCam, чтобы проверять плагин OBS без настоящего телефона.
// Тестовую картинку и тон 440 Гц генерирую через ffmpeg и отдаю по протоколу docs/PROTOCOL.md.
// Запуск: node tools/fake-phone.js [port] [--key 123456] [--stall] [--bframes [--no-reorder-flag]] [--old]
// С --key требую код доступа даже с localhost, иначе проверить его на одном компьютере нельзя.
// --bframes: кодирую с B-кадрами (как режим «Качество» на телефоне), pts беру из MPEG-TS, кадры идут в порядке
// декодирования и с флагом B-кадров в VIDEO_CONFIG; --no-reorder-flag этот флаг не ставит (так видно, что будет
// у клиента, который о B-кадрах не знает). --old: веду себя как телефон до bitrate=auto и enc.
'use strict';
const net = require('net');
const dgram = require('dgram');
const { spawn } = require('child_process');
const os = require('os');

const args = process.argv.slice(2);
const keyAt = args.indexOf('--key');
const KEY = keyAt >= 0 ? String(args[keyAt + 1] || '') : '';
const PORT = Number(args.find((a, i) => /^\d+$/.test(a) && (keyAt < 0 || i !== keyAt + 1)) || 4848);
const STALL = args.includes('--stall');
const BFRAMES = args.includes('--bframes');
const REORDER_FLAG = !args.includes('--no-reorder-flag');
const OLD = args.includes('--old');
const state = {
  name: 'Fake Phone (' + os.hostname() + ')', ver: 2, auth: !!KEY,
  profile: '', codec: 'h264', codec_pref: 'auto', hevc: true, abr: true, bitrate_now: 4000,
  lock: false, framing: false, framing_available: true, faces: false,
  bg: 'off', bg_strength: 0.6, bg_image: false, rec: false, hot: false, heat_guard: true,
  w: 1280, h: 720, fps: 30, bitrate: 4000, camera: '0', front: false,
  bitrate_auto: false, enc: 'latency', bframes: false,
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

function packet(type, payload, pts, key, hevc, reorder) {
  const h = Buffer.alloc(16);
  h.write('MC', 0, 'ascii');
  h[2] = type; h[3] = (key ? 1 : 0) | (hevc ? 2 : 0) | (reorder ? 4 : 0);
  h.writeUInt32BE(payload.length, 4);
  h.writeBigInt64BE(BigInt(pts), 8);
  return Buffer.concat([h, payload]);
}

// та же формула, что Core.autoBitrate на телефоне
function autoBitrate(w, h, fps, hevc) {
  let k = 12000 * Math.pow(w * h / 2073600, 0.9) * Math.pow(fps / 30, 0.6);
  if (hevc) k *= 0.65;
  return Math.min(50000, Math.max(2000, Math.round(k / 500) * 500));
}

function applyParams(q) {
  for (const [k, v] of q) {
    if (k === 'w') state.w = +v; else if (k === 'h') state.h = +v;
    else if (k === 'fps') state.fps = +v;
    else if (k === 'bitrate') {
      // старый телефон делал toIntOrNull(): "auto" не число, значит, не трогаю ничего
      if (v === 'auto') { if (!OLD) state.bitrate_auto = true; }
      else if (Number.isFinite(+v) && +v > 0) { state.bitrate = +v; state.bitrate_auto = false; }
    }
    else if (k === 'enc') { if (!OLD && (v === 'latency' || v === 'quality')) state.enc = v; }
    else if (k === 'bframes') {}
    else if (k === 'camera') {
      const c = state.cameras.find((x) => x.id === v);
      if (c) { state.camera = c.id; state.front = c.front; }
      else { state.front = v === 'front'; state.camera = state.front ? '1' : '0'; }
    }
    else if (k === 'audio') state.audio = v === '1';
    else if (k === 'torch') state.torch = v === 'toggle' ? !state.torch : v === '1';
    else if (k === 'zoom') state.zoom = +v; else if (k === 'ev') state.ev = +v;
    else if (k === 'orient') state.orient = +v;
    else if (['lock', 'framing', 'abr', 'heat_guard'].includes(k)) state[k] = v === '1' || v === 'true';
    else if (k === 'bg_strength') state.bg_strength = +v;
    else if (k === 'rec') { state.rec = v === '1'; recStart = Date.now(); }
    else if (k === 'profile' || k === 'codec' || k === 'key') { if (k === 'profile') state.profile = v; }
    else if (k in state) state[k] = v;
  }
}

let recStart = 0;
const snapshot = () => {
  if (state.rec) state.rec_ms = Date.now() - recStart; else delete state.rec_ms;
  if (state.bitrate_auto) state.bitrate = autoBitrate(state.w, state.h, state.fps, state.codec === 'hevc');
  state.bitrate_now = state.bitrate;
  const out = { ...state };
  if (OLD) { delete out.bitrate_auto; delete out.enc; delete out.bframes; }
  return Buffer.from(JSON.stringify(out));
};

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
  const hevc = query.get('codec') === 'hevc';
  applyParams(query);
  state.codec = hevc ? 'hevc' : 'h264';
  state.clients++;
  if (state.bitrate_auto) state.bitrate = autoBitrate(state.w, state.h, state.fps, hevc);
  state.bframes = BFRAMES;
  console.log(`[stream] client ${sock.remoteAddress} ${state.w}x${state.h}@${state.fps} ${state.bitrate}kbps` +
    `${state.bitrate_auto ? ' (auto)' : ''} front=${state.front} ${state.codec} enc=${state.enc}` +
    ` client-bframes=${query.get('bframes') === '1'} sending-bframes=${BFRAMES} reorder-flag=${BFRAMES && REORDER_FLAG}`);
  sock.write('HTTP/1.1 200 OK\r\nContent-Type: application/x-magercam\r\n\r\n');
  // --stall: каждые 15 с на 3 с придерживаю пакеты, потом отдаю пачкой — так выглядит пробка в Wi-Fi
  let held = null;
  const send = (b) => { if (sock.destroyed) return; if (held) held.push(b); else sock.write(b); };
  const stall = STALL ? setInterval(() => {
    held = [];
    setTimeout(() => { const h = held; held = null; console.log(`[stall] flushing ${h.length} packets`); for (const b of h) send(b); }, 3000);
  }, 15000) : null;
  sock.on('close', () => clearInterval(stall));

  const label = (state.front ? 'FRONT' : 'BACK') + ' cam ' + state.camera.replace(':', '-');
  const text = `MagerCam ${label} %{localtime\\:%T}` + (BFRAMES ? ' #%{n}' : '');
  // С B-кадрами беру MPEG-TS: в сыром .h264 нет меток, а порядок показа надо знать точно, как его знает MediaCodec.
  // Lookahead выключаю: телефонный кодер его не держит, и задержка тогда только от самих B-кадров.
  const venc = hevc
    ? ['-c:v', 'libx265', '-preset', 'ultrafast', ...(BFRAMES ? [] : ['-tune', 'zerolatency']), '-b:v', `${state.bitrate}k`,
      '-x265-params', `keyint=${state.fps}:aud=1:repeat-headers=1:log-level=error` +
        (BFRAMES ? ':bframes=2:b-adapt=0:open-gop=0:rc-lookahead=3:frame-threads=1' : '')]
    : ['-c:v', 'libx264', ...(BFRAMES
      ? ['-preset', 'veryfast', '-profile:v', 'high', '-bf', '2']
      : ['-preset', 'ultrafast', '-tune', 'zerolatency', '-profile:v', 'baseline']),
    '-b:v', `${state.bitrate}k`, '-g', String(state.fps), '-x264-params',
    'aud=1:repeat-headers=1' + (BFRAMES ? ':b-adapt=0:rc-lookahead=0:sync-lookahead=0:mbtree=0:threads=1' : '')];
  const vf = spawn('ffmpeg', ['-hide_banner', '-loglevel', 'error', '-re', '-f', 'lavfi',
    '-i', `testsrc2=size=${state.w}x${state.h}:rate=${state.fps}`,
    '-vf', `drawtext=fontfile=arial.ttf:text='${text}':fontsize=${Math.round(state.h / 12)}:fontcolor=white:box=1:boxcolor=black@0.6:x=(w-tw)/2:y=h*0.1`,
    ...venc, ...(BFRAMES ? ['-f', 'mpegts', '-flush_packets', '1', '-muxdelay', '0', '-muxpreload', '0'] : ['-f', hevc ? 'hevc' : 'h264']), '-'],
    { cwd: process.env.WINDIR ? process.env.WINDIR + '/Fonts' : undefined });
  let vbuf = Buffer.alloc(0), au = [], sentConfig = false;
  // тип NAL: у H.264 младшие 5 бит, у H.265 шесть бит после первого
  const nalType = (n) => { const b = n[n[2] === 1 ? 3 : 4]; return hevc ? (b >> 1) & 0x3f : b & 0x1f; };
  const T = hevc ? { aud: 35, cfg: [32, 33, 34], key: [19, 20, 21] } : { aud: 9, cfg: [7, 8], key: [5] };
  let sent = 0, backwards = 0, lastPts = -1;
  const flushAU = (pts) => {
    if (!au.length) return;
    const types = au.map(nalType);
    if (!sentConfig && types.includes(T.cfg[0])) {
      const cfg = Buffer.concat(au.filter((n, i) => T.cfg.includes(types[i])));
      send(packet(1, cfg, pts, true, hevc, BFRAMES && REORDER_FLAG));
      sentConfig = true;
    }
    if (sentConfig) {
      send(packet(2, Buffer.concat(au), pts, types.some((t) => T.key.includes(t))));
      if (pts < lastPts) backwards++;
      lastPts = pts;
      if (++sent % 300 === 0) console.log(`[video] ${sent} frames sent, ${backwards} of them after a frame with a later pts`);
    }
    au = [];
  };
  if (BFRAMES) {
    // Минимальный разбор MPEG-TS: видео у ffmpeg всегда на PID 0x100. Конец PES узнаю только по началу следующего,
    // поэтому кадр уходит на один кадр позже — для проверки это неважно.
    let ts = Buffer.alloc(0), pes = null, base = null;
    const ts90 = (b, o) => ((b[o] >> 1) & 7) * 1073741824 + ((b[o + 1] << 7) | (b[o + 2] >> 1)) * 32768 + ((b[o + 3] << 7) | (b[o + 4] >> 1));
    const finishPes = () => {
      if (!pes) return;
      const buf = Buffer.concat(pes.parts);
      const hl = buf[8];
      const pts90 = ts90(buf, 9);
      const dts90 = buf[7] & 0x40 ? ts90(buf, 14) : pts90;
      // первому кадру даю метку «сейчас минус два кадра»: настоящий телефон тоже отдаёт кадр позже, чем снял
      if (base === null) base = { us: nowUs() - Math.round(2e6 / state.fps), dts: dts90 };
      const pts = base.us + Math.round((pts90 - base.dts) * 100 / 9);
      au = [...nals(buf.subarray(9 + hl))].map((n) => Buffer.from(n));
      flushAU(pts);
      pes = null;
    };
    vf.stdout.on('data', (d) => {
      ts = Buffer.concat([ts, d]);
      let o = 0;
      for (; o + 188 <= ts.length; o += 188) {
        if (ts[o] !== 0x47) { o -= 187; continue; }
        const pid = ((ts[o + 1] & 0x1f) << 8) | ts[o + 2];
        if (pid !== 0x100) continue;
        const afc = (ts[o + 3] >> 4) & 3;
        if (!(afc & 1)) continue;
        const start = o + 4 + (afc & 2 ? 1 + ts[o + 4] : 0);
        if (ts[o + 1] & 0x40) { finishPes(); pes = { parts: [] }; }
        if (pes && start < o + 188) pes.parts.push(Buffer.from(ts.subarray(start, o + 188)));
      }
      ts = ts.subarray(o);
    });
  } else {
    vf.stdout.on('data', (d) => {
      vbuf = Buffer.concat([vbuf, d]);
      const units = [...nals(vbuf)];
      // последний NAL может быть недописан, поэтому держу его в буфере до следующего куска
      const last = units.pop();
      vbuf = last ? Buffer.from(last) : Buffer.alloc(0);
      for (const n of units) {
        if (nalType(n) === T.aud) flushAU(nowUs());
        au.push(Buffer.from(n));
      }
    });
  }
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

  const status = setInterval(() => send(packet(5, snapshot(), nowUs(), false)), 1000);
  send(packet(5, snapshot(), nowUs(), false));

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
    const hk = /\r\nX-MagerCam-Key:\s*([^\r]*)/i.exec(head);
    const given = url.searchParams.get('key') || (hk ? hk[1].trim() : '');
    if (KEY && given !== KEY) {
      console.log('[http] 401: bad key');
      sock.end('HTTP/1.0 401 Unauthorized\r\nContent-Length: 0\r\n\r\n');
      return;
    }
    if (url.pathname === '/stream') return startStream(sock, url.searchParams);
    if (url.pathname === '/api/set') applyParams(url.searchParams);
    if (url.pathname.startsWith('/api/')) {
      const body = snapshot().toString();
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
  if (!msg.toString().startsWith('MAGERCAM_DISCOVER')) return;
  console.log(`[discovery] from ${rinfo.address}:${rinfo.port}`);
  udp.send(JSON.stringify({ magercam: 1, name: state.name, port: PORT, ver: 2, auth: !!KEY }), rinfo.port, rinfo.address);
});
udp.bind(4849);
