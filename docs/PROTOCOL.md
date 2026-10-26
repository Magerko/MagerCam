# MagerCam protocol v1

Телефон — сервер. ПК (плагин OBS, браузер, VLC) — клиенты.

| Порт | Транспорт | Назначение |
|------|-----------|------------|
| 4848 | TCP (HTTP/1.0-подобный) | всё: веб-панель, MJPEG, API, бинарный поток для OBS |
| 4849 | UDP | автопоиск телефонов в локальной сети |

По USB работает тот же TCP-порт через `adb forward tcp:4848 tcp:4848`.

## HTTP endpoints (порт 4848)

| Запрос | Ответ |
|--------|-------|
| `GET /` | Веб-панель управления (превью + все настройки) |
| `GET /video` | MJPEG `multipart/x-mixed-replace` (браузер, VLC, OBS Media Source) |
| `GET /shot.jpg` | Один JPEG-кадр |
| `GET /api/status` | JSON-состояние (см. ниже) |
| `GET /api/set?k=v&k2=v2` | Изменить настройки, ответ — JSON-состояние |
| `GET /stream?<params>` | Бинарный поток H.264 + PCM для плагина OBS |

### Параметры `/stream` и `/api/set`

Все необязательные. Неизвестные игнорируются.

| Ключ | Значения | Живое изменение |
|------|----------|-----------------|
| `w`, `h` | разрешение, напр. `1920`,`1080` | перезапуск энкодера |
| `fps` | 15..60 | перезапуск энкодера |
| `bitrate` | кбит/с, 500..50000 | на лету |
| `camera` | `back`, `front`, либо id камеры Camera2 | перезапуск энкодера |
| `audio` | `0`/`1` | на лету |
| `torch` | `0`/`1`/`toggle` | на лету |
| `zoom` | 1.0..max | на лету |
| `ev` | компенсация экспозиции в EV, -3..3 | на лету |
| `focus` | `continuous`, `auto`, `infinity`, `macro`, `manual` | на лету |
| `focus_dist` | 0..1 (0=бесконечность, 1=ближайший), для `manual` | на лету |
| `af` | `trigger` — однократная фокусировка; `x`,`y` (0..1) — точка | на лету |
| `wb` | `auto`, `daylight`, `cloudy`, `incandescent`, `fluorescent`, `shade`, `twilight` | на лету |
| `ae_lock`, `awb_lock` | `0`/`1` | на лету |
| `antibanding` | `auto`, `50`, `60`, `off` | на лету |
| `stab` | `0`/`1` — стабилизация видео | на лету |
| `mirror` | `0`/`1` — только превью/MJPEG на телефоне | на лету |

## Бинарный поток `/stream`

Клиент шлёт `GET /stream?... HTTP/1.1\r\n\r\n`.
Телефон отвечает `HTTP/1.1 200 OK\r\nContent-Type: application/x-magercam\r\n\r\n`, затем пакеты:

```
offset size  field
0      2     magic  'M','C'
2      1     type
3      1     flags  (bit0 = keyframe)
4      4     length (uint32 big-endian, размер payload)
8      8     pts    (int64 big-endian, микросекунды, общий таймлайн видео+аудио)
16     N     payload
```

| type | Имя | payload |
|------|-----|---------|
| 1 | VIDEO_CONFIG | H.264 SPS+PPS, Annex-B. Шлётся при каждом (пере)запуске энкодера |
| 2 | VIDEO_FRAME  | H.264 access unit, Annex-B |
| 3 | AUDIO_CONFIG | uint32 BE sample_rate, uint8 channels (PCM s16le) |
| 4 | AUDIO        | PCM s16le interleaved |
| 5 | STATUS       | JSON (как `/api/status`), ~1 раз в секунду и при изменениях |
| 6 | PING         | пусто, ~1 раз в секунду если нет других данных |

После подключения нового клиента телефон запрашивает keyframe у энкодера.

## STATUS / `/api/status`

```json
{
  "name": "Pixel 7", "ver": 1,
  "w": 1920, "h": 1080, "fps": 30, "bitrate": 8000,
  "camera": "0", "front": false, "cameras": [{"id":"0","front":false,"label":"Основная"}],
  "orient": 0,          // на сколько градусов по часовой повернуть кадр, чтобы он стал ровным
  "audio": true, "torch": false, "zoom": 1.0, "zoom_max": 8.0,
  "ev": 0.0, "ev_min": -2.0, "ev_max": 2.0,
  "focus": "continuous", "wb": "auto", "antibanding": "auto", "stab": false,
  "battery": 84, "charging": true, "temp": 31.5,
  "clients": 1
}
```

## Автопоиск (UDP 4849)

ПК шлёт broadcast-датаграмму `MAGERCAM_DISCOVER`. Телефон отвечает unicast на адрес/порт
отправителя JSON: `{"magercam":1,"name":"Pixel 7","port":4848}`.
