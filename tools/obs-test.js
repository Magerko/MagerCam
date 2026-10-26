// Автотест плагина через obs-websocket v5: создаёт источник MagerCam, снимает скриншоты, крутит настройки.
// Запуск: node tools/obs-test.js [ws://127.0.0.1:4466] [password] [outdir]
'use strict';
const crypto = require('crypto');
const fs = require('fs');
const path = require('path');

const URL_ = process.argv[2] || 'ws://127.0.0.1:4466';
const PASSWORD = process.argv[3] || '';
const OUT = process.argv[4] || '.';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

function connect() {
  return new Promise((resolve, reject) => {
    const ws = new WebSocket(URL_);
    const pending = new Map();
    let id = 0;
    const call = (requestType, requestData = {}) => new Promise((res, rej) => {
      const requestId = String(++id);
      pending.set(requestId, { res, rej });
      ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
    });
    ws.onerror = (e) => reject(new Error('ws error ' + (e.message || '')));
    ws.onmessage = (ev) => {
      const m = JSON.parse(ev.data);
      if (m.op === 0) {
        const d = { rpcVersion: 1 };
        if (m.d.authentication) {
          const { salt, challenge } = m.d.authentication;
          const secret = crypto.createHash('sha256').update(PASSWORD + salt).digest('base64');
          d.authentication = crypto.createHash('sha256').update(secret + challenge).digest('base64');
        }
        ws.send(JSON.stringify({ op: 1, d }));
      } else if (m.op === 2) resolve({ call, ws });
      else if (m.op === 7) {
        const p = pending.get(m.d.requestId);
        if (!p) return;
        pending.delete(m.d.requestId);
        if (m.d.requestStatus.result) p.res(m.d.responseData || {});
        else p.rej(new Error(`${m.d.requestType}: ${m.d.requestStatus.code} ${m.d.requestStatus.comment || ''}`));
      }
    };
  });
}

async function shot(call, name, file) {
  const r = await call('GetSourceScreenshot', { sourceName: name, imageFormat: 'png', imageWidth: 640 });
  const buf = Buffer.from(r.imageData.split(',')[1], 'base64');
  fs.writeFileSync(path.join(OUT, file), buf);
  return buf.length;
}

(async () => {
  const { call, ws } = await connect();
  const ver = await call('GetVersion');
  console.log('OBS', ver.obsVersion, 'ws', ver.obsWebSocketVersion);
  const kinds = (await call('GetInputKindList')).inputKinds;
  console.log('magercam_source registered:', kinds.includes('magercam_source'));
  if (!kinds.includes('magercam_source')) process.exit(2);

  const scene = 'MagerCam Test';
  const input = 'MagerCam';
  try { await call('CreateScene', { sceneName: scene }); } catch {}
  await call('SetCurrentProgramScene', { sceneName: scene });
  try { await call('RemoveInput', { inputName: input }); await sleep(500); } catch {}
  const created = await call('CreateInput', {
    sceneName: scene, inputName: input, inputKind: 'magercam_source',
    inputSettings: { host: '127.0.0.1', port: 4848, resolution: '1280x720', fps: 30, bitrate: 4 },
  });
  const itemId = created.sceneItemId;
  await call('SetSceneItemTransform', { sceneName: scene, sceneItemId: itemId,
    sceneItemTransform: { boundsType: 'OBS_BOUNDS_SCALE_INNER', boundsWidth: 1920, boundsHeight: 1080 } });

  const dims = async () => {
    const t = (await call('GetSceneItemTransform', { sceneName: scene, sceneItemId: itemId })).sceneItemTransform;
    return `${t.sourceWidth}x${t.sourceHeight}`;
  };

  await sleep(4000);
  console.log('source size after connect:', await dims());
  console.log('shot1 bytes', await shot(call, input, 'shot1-landscape.png'));

  await call('SetInputSettings', { inputName: input, inputSettings: { rotation: 90 } });
  await sleep(1500);
  console.log('size after rotation=90:', await dims());
  console.log('shot2 bytes', await shot(call, input, 'shot2-rot90.png'));

  await call('SetInputSettings', { inputName: input, inputSettings: { rotation: -1, mirror: true, zoom: 2.5, torch: 1 } });
  await sleep(1500);
  console.log('shot3 bytes', await shot(call, input, 'shot3-mirror.png'));

  await call('SetInputSettings', { inputName: input, inputSettings: { mirror: false, resolution: '1920x1080', camera: 1 } });
  await sleep(4000);
  console.log('size after 1080p+front:', await dims());
  console.log('shot4 bytes', await shot(call, input, 'shot4-1080-front.png'));

  await call('PressInputPropertiesButton', { inputName: input, propertyName: 'torch' });
  await call('PressInputPropertiesButton', { inputName: input, propertyName: 'discover' });
  const items = await call('GetInputPropertiesListPropertyItems', { inputName: input, propertyName: 'device' });
  console.log('discovered:', items.propertyItems.map((i) => `${i.itemName} => ${i.itemValue}`).join(' | '));

  const vol = await call('GetInputVolume', { inputName: input });
  console.log('audio volume dB:', vol.inputVolumeDb);

  await call('SetInputSettings', { inputName: input, inputSettings: { hw_decode: true } });
  await sleep(4000);
  console.log('shot5 (hw decode) bytes', await shot(call, input, 'shot5-hw.png'));
  ws.close();
  console.log('DONE');
})().catch((e) => { console.error('FAIL', e.message); process.exit(1); });
