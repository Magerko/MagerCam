#define COBJMACROS
#include "net.h"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <mfapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "decoder.h"
#include "audio.h"
#include "vcam.h"
#include "color.h"
#include "pair.h"
#include "../common/mcv_nv12.h"
#include "../common/mcv_shared.h"

#define APP_VERSION "1.1.0"
#define WM_APP_FRAME (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_FOUND (WM_APP + 3)
#define WM_APP_INSTALLED (WM_APP + 4)
#define WM_APP_TRAY (WM_APP + 5)
#define WM_APP_PAIRED (WM_APP + 6)
#define WM_APP_BG_SENT (WM_APP + 7)
#define TIMER_CONTROLS 1
#define TIMER_VCAM 2
#define TIMER_COLOR 3
#define PREVIEW_W 640
#define PREVIEW_H 360
#define MAX_CAMS 16

enum {
	ID_WIFI = 100, ID_USB, ID_HOST, ID_FIND, ID_CONNECT, ID_AUTOCONNECT,
	ID_CAMERA, ID_RES, ID_FPS, ID_BITRATE, ID_TORCH, ID_AF, ID_ZOOM, ID_EV, ID_ZOOM_LABEL, ID_EV_LABEL,
	ID_ROT, ID_MIRROR, ID_FILL, ID_AUDIO, ID_AUDIO_DEV, ID_STATUS, ID_VCAM_STATE, ID_INSTALL, ID_AUTOSTART,
	ID_PREVIEW, ID_TOTRAY,
	ID_KEY, ID_PAIR, ID_HEVC, ID_MORE,
	ID_PROFILE, ID_BG, ID_BG_STRENGTH, ID_BG_PICK, ID_FRAMING, ID_LOCK, ID_ABR, ID_HEAT, ID_REC,
	ID_BRIGHT, ID_CONTRAST, ID_SAT, ID_WARM, ID_DOC, ID_LUT, ID_COLOR_RESET,
	ID_TRAY_OPEN = 300, ID_TRAY_CONNECT, ID_TRAY_EXIT,
};

/* чем закончилась сессия: после отказа в коде доступа не долблю телефон повторами,
 * иначе он закроется от меня на минуту */
enum { SESSION_RETRY, SESSION_AUTH_FAILED };

struct settings {
	int conn; /* 0 — Wi-Fi, 1 — USB */
	char host[128];
	int port;
	char camera[64];
	int width, height, fps, bitrate_kbps;
	int rotation; /* -1 — беру ориентацию с телефона */
	bool mirror, fill;
	bool audio;
	wchar_t audio_dev[256];
	bool autoconnect;
	bool to_tray; /* закрытие и сворачивание прячут окно в трей */
	char key[16];  /* код доступа с экрана телефона */
	bool hevc;     /* просить H.265, если в Windows есть декодер */
	struct color_params color;
	wchar_t lut_path[MAX_PATH];
};

struct cam_entry {
	char id[64];
	wchar_t label[96];
};

struct phone_status {
	bool valid;
	wchar_t name[96];
	int w, h, fps, battery, orient;
	bool charging, torch, has_flash;
	char camera[64];
	struct cam_entry cams[MAX_CAMS];
	int ncams;
	double zoom, zoom_min, zoom_max, ev, ev_min, ev_max;
	wchar_t error[160];
	/* протокол v2 */
	char profile[16], codec[8], bg[8];
	bool abr, lock, framing, framing_available, faces, bg_image, rec, hot, heat_guard;
	int bitrate_now;
	double bg_strength;
	long long rec_ms;
	wchar_t bg_error[120], rec_error[120];
};

static struct {
	HINSTANCE inst;
	HWND wnd, preview, status, vcam_label, install_btn;
	HWND host, connect, camera, res, fps, bitrate, zoom, ev, zoom_label, ev_label, rot, mirror, fill, audio, audio_dev;
	HWND wifi, usb, autoconnect, autostart, totray, torch;
	HWND key, hevc, profile, bg, bg_strength, bg_strength_label, framing, lock_btn, abr, heat, rec, phone_info;
	HWND bright, contrast, sat, warm, doc, lut_btn;
	NOTIFYICONDATAW nid;
	int slot;              /* номер камеры: 0 — «MagerCam», 1 — «MagerCam 2»… */
	wchar_t section[32];   /* секция client.ini для этой камеры */
	wchar_t cam_name[32];
	int conn_port;         /* порт, по которому реально идёт связь (у USB он локальный) */
	struct color_table *color; /* под lock */
	struct color_lut *lut;
	uint8_t *cc_buf;
	size_t cc_cap;
	bool bg_dirty;
	UINT taskbar_created;
	bool tray_hint_shown;
	HFONT font, font_bold;
	int dpi;

	/* cfg, ph, net_status, connected, превью и sock трогаю только под lock */
	CRITICAL_SECTION lock;
	struct settings cfg;
	struct phone_status ph;
	wchar_t net_status[256];
	bool connected;
	uint32_t *preview_bgra;
	bool preview_valid;

	volatile bool want_connect;
	volatile bool quit;
	volatile LONG gen;
	HANDLE wake, net_thread;
	SOCKET sock;

	enum vcam_state vcam;
	long vcam_hr;
	bool vcam_in_use;

	struct audio_device devs[AUDIO_MAX_DEVICES];
	int ndevs;
	struct found_device found[32];
	int nfound;
	bool zoom_dirty, ev_dirty, updating_ui;
	wchar_t ini[MAX_PATH];
} app;

#define S(x) MulDiv((x), app.dpi, 96)

static void ini_get(const wchar_t *key, const wchar_t *def, wchar_t *out, int n)
{
	GetPrivateProfileStringW(app.section, key, def, out, n, app.ini);
}

static int ini_int(const wchar_t *key, int def)
{
	return (int)GetPrivateProfileIntW(app.section, key, def, app.ini);
}

static void ini_set(const wchar_t *key, const wchar_t *val)
{
	WritePrivateProfileStringW(app.section, key, val, app.ini);
}

static void ini_set_int(const wchar_t *key, int v)
{
	wchar_t b[32];
	_snwprintf(b, 32, L"%d", v);
	ini_set(key, b);
}

static void load_settings(void)
{
	wchar_t dir[MAX_PATH];
	if (SUCCEEDED(SHGetFolderPathW(NULL, CSIDL_APPDATA, NULL, 0, dir))) {
		_snwprintf(app.ini, MAX_PATH, L"%ls\\MagerCam", dir);
		CreateDirectoryW(app.ini, NULL);
		_snwprintf(app.ini, MAX_PATH, L"%ls\\MagerCam\\client.ini", dir);
		if (GetFileAttributesW(app.ini) == INVALID_FILE_ATTRIBUTES) {
			/* создаю файл заранее в UTF-16 с BOM: только тогда WritePrivateProfileStringW
			 * пишет Unicode, иначе кириллица в настройках ломается */
			HANDLE f = CreateFileW(app.ini, GENERIC_WRITE, 0, NULL, CREATE_NEW, 0, NULL);
			if (f != INVALID_HANDLE_VALUE) {
				DWORD w;
				WriteFile(f, "\xFF\xFE", 2, &w, NULL);
				CloseHandle(f);
			}
		}
	}
	struct settings *c = &app.cfg;
	wchar_t b[256];
	c->conn = ini_int(L"conn", 0);
	ini_get(L"host", L"", b, 256);
	wide_to_utf8(b, c->host, sizeof(c->host));
	c->port = ini_int(L"port", MC_DEFAULT_PORT);
	ini_get(L"camera", L"back", b, 256);
	wide_to_utf8(b, c->camera, sizeof(c->camera));
	c->width = ini_int(L"width", 1920);
	c->height = ini_int(L"height", 1080);
	c->fps = ini_int(L"fps", 30);
	c->bitrate_kbps = ini_int(L"bitrate", 8000);
	c->rotation = ini_int(L"rotation", -1);
	c->mirror = ini_int(L"mirror", 0) != 0;
	c->fill = ini_int(L"fill", 0) != 0;
	c->audio = ini_int(L"audio", 0) != 0;
	ini_get(L"audio_dev", L"", c->audio_dev, 256);
	c->autoconnect = ini_int(L"autoconnect", 1) != 0;
	c->to_tray = ini_int(L"to_tray", 1) != 0;
	ini_get(L"key", L"", b, 256);
	wide_to_utf8(b, c->key, sizeof(c->key));
	c->hevc = ini_int(L"hevc", 1) != 0;
	c->color.brightness = ini_int(L"brightness", 0);
	c->color.contrast = ini_int(L"contrast", 0);
	c->color.saturation = ini_int(L"saturation", 0);
	c->color.warmth = ini_int(L"warmth", 0);
	c->color.doc = ini_int(L"doc", 0) != 0;
	ini_get(L"lut", L"", c->lut_path, MAX_PATH);
}

static void save_settings(void)
{
	struct settings c;
	EnterCriticalSection(&app.lock);
	c = app.cfg;
	LeaveCriticalSection(&app.lock);
	wchar_t *w;
	ini_set_int(L"conn", c.conn);
	w = utf8_to_wide(c.host);
	ini_set(L"host", w);
	free(w);
	ini_set_int(L"port", c.port);
	w = utf8_to_wide(c.camera);
	ini_set(L"camera", w);
	free(w);
	ini_set_int(L"width", c.width);
	ini_set_int(L"height", c.height);
	ini_set_int(L"fps", c.fps);
	ini_set_int(L"bitrate", c.bitrate_kbps);
	ini_set_int(L"rotation", c.rotation);
	ini_set_int(L"mirror", c.mirror);
	ini_set_int(L"fill", c.fill);
	ini_set_int(L"audio", c.audio);
	ini_set(L"audio_dev", c.audio_dev);
	ini_set_int(L"autoconnect", c.autoconnect);
	ini_set_int(L"to_tray", c.to_tray);
	w = utf8_to_wide(c.key);
	ini_set(L"key", w);
	free(w);
	ini_set_int(L"hevc", c.hevc);
	ini_set_int(L"brightness", c.color.brightness);
	ini_set_int(L"contrast", c.color.contrast);
	ini_set_int(L"saturation", c.color.saturation);
	ini_set_int(L"warmth", c.color.warmth);
	ini_set_int(L"doc", c.color.doc);
	ini_set(L"lut", c.lut_path);
}

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool autostart_enabled(void)
{
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
		return false;
	LSTATUS r = RegQueryValueExW(k, app.cam_name, NULL, NULL, NULL, NULL);
	RegCloseKey(k);
	return r == ERROR_SUCCESS;
}

static void set_autostart(bool on)
{
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
		return;
	if (on) {
		wchar_t exe[MAX_PATH], cmd[MAX_PATH + 48];
		GetModuleFileNameW(NULL, exe, MAX_PATH);
		if (app.slot)
			_snwprintf(cmd, MAX_PATH + 48, L"\"%ls\" --minimized --slot %d", exe, app.slot + 1);
		else
			_snwprintf(cmd, MAX_PATH + 48, L"\"%ls\" --minimized", exe);
		RegSetValueExW(k, app.cam_name, 0, REG_SZ, (const BYTE *)cmd, (DWORD)(wcslen(cmd) + 1) * 2);
	} else {
		RegDeleteValueW(k, app.cam_name);
	}
	RegCloseKey(k);
}

/* таблицу пересобираю в UI-потоке, а подменяю под lock: кадры идут из сетевого потока */
static void rebuild_color(void)
{
	EnterCriticalSection(&app.lock);
	struct color_params p = app.cfg.color;
	LeaveCriticalSection(&app.lock);
	struct color_table *t = color_table_build(&p, app.lut);
	EnterCriticalSection(&app.lock);
	struct color_table *old = app.color;
	app.color = t;
	LeaveCriticalSection(&app.lock);
	color_table_free(old);
}

static bool load_lut(const wchar_t *path, char *err, size_t errsz)
{
	struct color_lut *l = NULL;
	if (path && *path) {
		l = color_lut_load(path, err, errsz);
		if (!l)
			return false;
	}
	color_lut_free(app.lut);
	app.lut = l;
	return true;
}

static void set_net_status(const wchar_t *fmt, ...)
{
	wchar_t buf[256];
	va_list ap;
	va_start(ap, fmt);
	_vsnwprintf(buf, 255, fmt, ap);
	va_end(ap);
	buf[255] = 0;
	EnterCriticalSection(&app.lock);
	bool changed = wcscmp(buf, app.net_status) != 0;
	wcscpy(app.net_status, buf);
	LeaveCriticalSection(&app.lock);
	if (changed)
		PostMessageW(app.wnd, WM_APP_STATUS, 0, 0);
}

static void set_net_status_utf8(const char *msg)
{
	wchar_t *w = utf8_to_wide(msg);
	set_net_status(L"%ls", w);
	free(w);
}

static void apply_status_json(const char *text)
{
	struct json *j = json_parse(text);
	if (!j)
		return;
	struct phone_status p = {0};
	p.valid = true;
	wchar_t *w = utf8_to_wide(json_str(j, "name", "Телефон"));
	wcsncpy(p.name, w, 95);
	free(w);
	p.w = (int)json_num(j, "w", 0);
	p.h = (int)json_num(j, "h", 0);
	p.fps = (int)json_num(j, "fps", 0);
	p.battery = (int)json_num(j, "battery", -1);
	p.charging = json_bool(j, "charging", false);
	p.orient = (int)json_num(j, "orient", 0);
	p.torch = json_bool(j, "torch", false);
	p.has_flash = json_bool(j, "has_flash", false);
	snprintf(p.camera, sizeof(p.camera), "%s", json_str(j, "camera", ""));
	p.zoom = json_num(j, "zoom", 1);
	p.zoom_min = json_num(j, "zoom_min", 1);
	p.zoom_max = json_num(j, "zoom_max", 1);
	p.ev = json_num(j, "ev", 0);
	p.ev_min = json_num(j, "ev_min", 0);
	p.ev_max = json_num(j, "ev_max", 0);
	const char *err = json_str(j, "error", NULL);
	if (err) {
		w = utf8_to_wide(err);
		wcsncpy(p.error, w, 159);
		free(w);
	}
	snprintf(p.profile, sizeof(p.profile), "%s", json_str(j, "profile", ""));
	snprintf(p.codec, sizeof(p.codec), "%s", json_str(j, "codec", "h264"));
	snprintf(p.bg, sizeof(p.bg), "%s", json_str(j, "bg", "off"));
	p.abr = json_bool(j, "abr", false);
	p.lock = json_bool(j, "lock", false);
	p.framing = json_bool(j, "framing", false);
	p.framing_available = json_bool(j, "framing_available", false);
	p.faces = json_bool(j, "faces", false);
	p.bg_image = json_bool(j, "bg_image", false);
	p.rec = json_bool(j, "rec", false);
	p.hot = json_bool(j, "hot", false);
	p.heat_guard = json_bool(j, "heat_guard", false);
	p.bitrate_now = (int)json_num(j, "bitrate_now", 0);
	p.bg_strength = json_num(j, "bg_strength", 0.6);
	p.rec_ms = (long long)json_num(j, "rec_ms", 0);
	if ((err = json_str(j, "bg_error", NULL)) != NULL) {
		w = utf8_to_wide(err);
		wcsncpy(p.bg_error, w, 119);
		free(w);
	}
	if ((err = json_str(j, "rec_error", NULL)) != NULL) {
		w = utf8_to_wide(err);
		wcsncpy(p.rec_error, w, 119);
		free(w);
	}
	const struct json *cams = json_get(j, "cameras");
	if (cams && cams->type == JSON_ARR) {
		for (const struct json *c = cams->child; c && p.ncams < MAX_CAMS; c = c->next) {
			snprintf(p.cams[p.ncams].id, sizeof(p.cams[0].id), "%s", json_str(c, "id", ""));
			w = utf8_to_wide(json_str(c, "label", "?"));
			wcsncpy(p.cams[p.ncams].label, w, 95);
			free(w);
			p.ncams++;
		}
	}
	json_free(j);
	EnterCriticalSection(&app.lock);
	app.ph = p;
	LeaveCriticalSection(&app.lock);
	PostMessageW(app.wnd, WM_APP_STATUS, 1, 0);
}

static void yuv_to_bgra(const struct nv12 *s, uint32_t *out)
{
	for (int r = 0; r < s->h; r++) {
		const uint8_t *yr = s->y + (size_t)r * s->ys, *uvr = s->uv + (size_t)(r / 2) * s->uvs;
		uint32_t *o = out + (size_t)r * s->w;
		for (int c = 0; c < s->w; c++) {
			int Y = (yr[c] - 16) * 298, U = uvr[c & ~1] - 128, V = uvr[(c & ~1) + 1] - 128;
			int R = (Y + 459 * V + 128) >> 8, G = (Y - 55 * U - 136 * V + 128) >> 8, B = (Y + 541 * U + 128) >> 8;
			R = R < 0 ? 0 : R > 255 ? 255 : R;
			G = G < 0 ? 0 : G > 255 ? 255 : G;
			B = B < 0 ? 0 : B > 255 ? 255 : B;
			o[c] = (uint32_t)(B | (G << 8) | (R << 16));
		}
	}
}

struct session {
	struct decoder *dec;
	struct audio_out *aout;
	int a_rate, a_ch;
	uint8_t *small;
	ULONGLONG last_preview;
	int frames;
	ULONGLONG fps_t0;
	int fps_count;
	double fps_now;
};

static void on_frame(void *opaque, const uint8_t *y, const uint8_t *uv, int stride, int w, int h)
{
	struct session *s = opaque;
	struct nv12 src = {(uint8_t *)y, (uint8_t *)uv, stride, stride, w, h};
	EnterCriticalSection(&app.lock);
	int rot = app.cfg.rotation < 0 ? app.ph.orient : app.cfg.rotation;
	bool mirror = app.cfg.mirror, fill = app.cfg.fill;
	/* цвет правлю один раз на исходном кадре, тогда и камера, и превью получают одно и то же */
	if (app.color) {
		size_t need = (size_t)w * h * 3 / 2;
		if (need > app.cc_cap) {
			free(app.cc_buf);
			app.cc_buf = malloc(need);
			app.cc_cap = app.cc_buf ? need : 0;
		}
		if (app.cc_buf) {
			struct nv12 d = {app.cc_buf, app.cc_buf + (size_t)w * h, w, w, w, h};
			color_apply(app.color, &src, &d);
			src = d;
		}
	}
	LeaveCriticalSection(&app.lock);

	vcam_write(&src, rot, mirror, fill);

	s->frames++;
	s->fps_count++;
	ULONGLONG now = GetTickCount64();
	if (now - s->fps_t0 >= 1000) {
		s->fps_now = s->fps_count * 1000.0 / (double)(now - s->fps_t0);
		s->fps_count = 0;
		s->fps_t0 = now;
	}
	/* превью хватает ~25 кадров в секунду, так меньше грузится процессор */
	if (now - s->last_preview < 38 || IsIconic(app.wnd) || !IsWindowVisible(app.wnd))
		return;
	s->last_preview = now;
	if (!s->small)
		s->small = malloc(PREVIEW_W * PREVIEW_H * 3 / 2);
	struct nv12 d = {s->small, s->small + PREVIEW_W * PREVIEW_H, PREVIEW_W, PREVIEW_W, PREVIEW_W, PREVIEW_H};
	nv12_transform(&src, &d, rot, mirror, fill);
	EnterCriticalSection(&app.lock);
	yuv_to_bgra(&d, app.preview_bgra);
	app.preview_valid = true;
	LeaveCriticalSection(&app.lock);
	PostMessageW(app.wnd, WM_APP_FRAME, 0, 0);
}

static void close_audio(struct session *s)
{
	audio_close(s->aout);
	s->aout = NULL;
}

static bool g_hevc_decoder; /* проверяю один раз при запуске */

static int stream_session(const struct settings *cfg, LONG gen)
{
	char host[128];
	snprintf(host, sizeof(host), "%s", cfg->host);
	int port = cfg->port;
	if (cfg->conn == 1) {
		char err[256];
		/* у каждой камеры свой локальный порт, чтобы два телефона по USB не мешали друг другу */
		port = cfg->port + 100 * app.slot;
		set_net_status(L"USB: подготовка adb…");
		if (!adb_prepare(NULL, port, cfg->port, err, sizeof(err))) {
			set_net_status_utf8(err);
			return SESSION_RETRY;
		}
		snprintf(host, sizeof(host), "127.0.0.1");
		/* по USB телефон под рукой: если MagerCam на нём не запущено, открываю его */
		set_net_status(L"USB: проверяю MagerCam на телефоне…");
		if (adb_ensure_app() == 1) {
			set_net_status(L"USB: MagerCam на телефоне запущено, жду камеру…");
			Sleep(2500);
		}
	} else if (!*host) {
		set_net_status(L"Укажите IP телефона, нажмите «Найти» или «QR-сопряжение»");
		return SESSION_RETRY;
	}
	EnterCriticalSection(&app.lock);
	app.conn_port = port;
	LeaveCriticalSection(&app.lock);

	wchar_t *whost = utf8_to_wide(host);
	set_net_status(L"Подключение к %ls:%d…", whost, port);
	SOCKET sock = tcp_connect(host, port, 3000);
	if (sock == INVALID_SOCKET) {
		set_net_status(L"Нет связи с %ls:%d. Запущено ли MagerCam на телефоне?", whost, port);
		free(whost);
		return SESSION_RETRY;
	}
	EnterCriticalSection(&app.lock);
	app.sock = sock;
	LeaveCriticalSection(&app.lock);

	set_recv_timeout(sock, 5000);
	char cam[200], req[1024], keyhdr[64] = "";
	url_encode(cam, sizeof(cam), cfg->camera);
	if (cfg->key[0])
		snprintf(keyhdr, sizeof(keyhdr), "X-MagerCam-Key: %s\r\n", cfg->key);
	int n = snprintf(req, sizeof(req),
			 "GET /stream?w=%d&h=%d&fps=%d&bitrate=%d&audio=%d&camera=%s%s HTTP/1.1\r\nHost: %s\r\n"
			 "User-Agent: MagerCam-Windows/" APP_VERSION "\r\n%s\r\n",
			 cfg->width, cfg->height, cfg->fps, cfg->bitrate_kbps, cfg->audio ? 1 : 0, cam,
			 cfg->hevc && g_hevc_decoder ? "&codec=hevc" : "", host, keyhdr);
	bool ok = send_all(sock, req, (size_t)n);
	int code = ok ? read_http_header(sock) : -1;
	int result = SESSION_RETRY;
	if (code == 409) {
		set_net_status(L"Телефон занят другим клиентом");
		ok = false;
	} else if (code == 401) {
		set_net_status(cfg->key[0] ? L"Неверный код доступа. Введите код с экрана телефона."
					   : L"Телефон просит код доступа: введите его в поле «Код» или сопрягитесь по QR.");
		result = SESSION_AUTH_FAILED;
		ok = false;
	} else if (code == 429) {
		set_net_status(L"Слишком много неверных кодов, телефон ждёт минуту. Проверьте код.");
		result = SESSION_AUTH_FAILED;
		ok = false;
	} else if (code != 200) {
		set_net_status(L"Неверный ответ телефона (%d)", code);
		ok = false;
	}

	struct session s = {0};
	s.a_rate = 48000;
	s.a_ch = 1;
	s.fps_t0 = GetTickCount64();
	uint8_t *payload = NULL;
	size_t cap = 0;
	if (ok) {
		EnterCriticalSection(&app.lock);
		app.connected = true;
		LeaveCriticalSection(&app.lock);
		set_net_status(L"Подключено к %ls", whost);
	}
	ULONGLONG last_ui = 0;
	wchar_t audio_dev[256] = L"";

	while (ok && !app.quit && app.gen == gen) {
		uint8_t hdr[16];
		if (!recv_all(sock, hdr, sizeof(hdr)))
			break;
		if (hdr[0] != 'M' || hdr[1] != 'C')
			break;
		int type = hdr[2], flags = hdr[3];
		uint32_t len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
		int64_t pts = 0;
		for (int i = 0; i < 8; i++)
			pts = (pts << 8) | hdr[8 + i];
		if (len > 32 * 1024 * 1024)
			break;
		if (len + 1 > cap) {
			free(payload);
			cap = (size_t)len * 2 + 1;
			payload = malloc(cap);
		}
		if (len && !recv_all(sock, payload, len))
			break;
		payload[len] = 0;

		switch (type) {
		case PKT_VIDEO_CONFIG:
			/* конфиг приходит заново при смене разрешения или камеры, поэтому декодер пересоздаю */
			decoder_destroy(s.dec);
			/* бит 1 во флагах конфига: телефон кодирует в H.265 */
			s.dec = decoder_create(on_frame, &s, (flags & 2) != 0);
			if (!s.dec) {
				set_net_status((flags & 2) ? L"Ошибка: нет декодера H.265. Отключите «H.265» или поставьте «Расширения для видео HEVC»."
							   : L"Ошибка: декодер H.264 Windows недоступен");
				ok = false;
				break;
			}
			decoder_feed(s.dec, payload, len, pts);
			break;
		case PKT_VIDEO_FRAME:
			if (s.dec && !decoder_feed(s.dec, payload, len, pts)) {
				decoder_destroy(s.dec);
				s.dec = NULL; /* жду следующий конфиг */
			}
			break;
		case PKT_AUDIO_CONFIG:
			if (len >= 5) {
				s.a_rate = (int)(((uint32_t)payload[0] << 24) | ((uint32_t)payload[1] << 16) |
						 ((uint32_t)payload[2] << 8) | payload[3]);
				s.a_ch = payload[4];
				close_audio(&s);
			}
			break;
		case PKT_AUDIO: {
			EnterCriticalSection(&app.lock);
			bool want = app.cfg.audio && app.cfg.audio_dev[0];
			bool dev_changed = wcscmp(audio_dev, app.cfg.audio_dev) != 0;
			wcscpy(audio_dev, app.cfg.audio_dev);
			LeaveCriticalSection(&app.lock);
			if (dev_changed || !want)
				close_audio(&s);
			if (want && !s.aout && s.a_ch >= 1 && s.a_ch <= 2)
				s.aout = audio_open(audio_dev, s.a_rate, s.a_ch);
			if (s.aout)
				audio_write(s.aout, (const int16_t *)payload, (int)(len / (2 * s.a_ch)));
			break;
		}
		case PKT_STATUS:
			apply_status_json((const char *)payload);
			break;
		default:
			break;
		}
		ULONGLONG now = GetTickCount64();
		if (now - last_ui > 1000) {
			last_ui = now;
			EnterCriticalSection(&app.lock);
			struct phone_status ph = app.ph;
			LeaveCriticalSection(&app.lock);
			if (ph.valid) {
				wchar_t bat[64] = L"";
				if (ph.battery >= 0)
					_snwprintf(bat, 64, L", батарея %d%%%ls", ph.battery, ph.charging ? L" ⚡" : L"");
				wchar_t br[48] = L"";
				if (ph.bitrate_now > 0)
					_snwprintf(br, 48, L", %ls %.1f Мбит/с", strcmp(ph.codec, "hevc") ? L"H.264" : L"H.265",
						   ph.bitrate_now / 1000.0);
				if (ph.error[0])
					set_net_status(L"%ls: %ls", ph.name, ph.error);
				else
					set_net_status(L"%ls — %dx%d, %.0f к/с%ls%ls", ph.name, ph.w, ph.h, s.fps_now, br, bat);
			}
		}
	}

	EnterCriticalSection(&app.lock);
	app.sock = INVALID_SOCKET;
	bool was = app.connected;
	app.connected = false;
	app.ph.valid = false;
	app.preview_valid = false;
	LeaveCriticalSection(&app.lock);
	closesocket(sock);
	decoder_destroy(s.dec);
	close_audio(&s);
	free(s.small);
	free(payload);
	if (was && !app.quit && app.gen == gen)
		set_net_status(L"Соединение потеряно, переподключение…");
	PostMessageW(app.wnd, WM_APP_FRAME, 0, 0);
	PostMessageW(app.wnd, WM_APP_STATUS, 1, 0);
	free(whost);
	return result;
}

static DWORD WINAPI net_main(void *arg)
{
	(void)arg;
	CoInitializeEx(NULL, COINIT_MULTITHREADED);
	MFStartup(MF_VERSION, MFSTARTUP_LITE);
	while (!app.quit) {
		if (!app.want_connect) {
			WaitForSingleObject(app.wake, 500);
			continue;
		}
		LONG gen = app.gen;
		EnterCriticalSection(&app.lock);
		struct settings cfg = app.cfg;
		LeaveCriticalSection(&app.lock);
		int r = stream_session(&cfg, gen);
		/* после отказа по коду жду, пока пользователь не поменяет код или не переподключится */
		while (r == SESSION_AUTH_FAILED && !app.quit && app.gen == gen && app.want_connect)
			WaitForSingleObject(app.wake, 500);
		if (!app.quit && app.gen == gen && app.want_connect)
			WaitForSingleObject(app.wake, 1500);
	}
	MFShutdown();
	CoUninitialize();
	return 0;
}

static void reconnect(void)
{
	InterlockedIncrement(&app.gen);
	EnterCriticalSection(&app.lock);
	if (app.sock != INVALID_SOCKET)
		shutdown(app.sock, SD_BOTH);
	LeaveCriticalSection(&app.lock);
	SetEvent(app.wake);
}

/* запросы /api/set гоняю в коротких потоках, чтобы интерфейс не подвисал */
struct control_req {
	char host[128];
	int port;
	char path[512];
	char key[16];
	uint8_t *body; /* для POST */
	size_t len;
};

static DWORD WINAPI control_main(void *p)
{
	struct control_req *r = p;
	int code = http_request(r->host, r->port, r->body ? "POST" : "GET", r->path, r->key, r->body, r->len);
	if (r->body)
		PostMessageW(app.wnd, WM_APP_BG_SENT, (WPARAM)code, 0);
	free(r->body);
	free(r);
	return 0;
}

static struct control_req *control_target(void)
{
	struct control_req *r = calloc(1, sizeof(*r));
	EnterCriticalSection(&app.lock);
	bool connected = app.connected;
	snprintf(r->host, sizeof(r->host), "%s", app.cfg.conn == 1 ? "127.0.0.1" : app.cfg.host);
	r->port = app.conn_port ? app.conn_port : app.cfg.port;
	snprintf(r->key, sizeof(r->key), "%s", app.cfg.key);
	LeaveCriticalSection(&app.lock);
	if (!connected || !r->host[0]) {
		free(r);
		return NULL;
	}
	return r;
}

static void control_start(struct control_req *r)
{
	HANDLE h = CreateThread(NULL, 0, control_main, r, 0, NULL);
	if (h) {
		CloseHandle(h);
	} else {
		free(r->body);
		free(r);
	}
}

/* картинку для замены фона отправляю телом POST /api/background как есть, телефон сам её декодирует */
static void send_background(const wchar_t *path)
{
	HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (f == INVALID_HANDLE_VALUE)
		return;
	LARGE_INTEGER sz;
	GetFileSizeEx(f, &sz);
	struct control_req *r = NULL;
	if (sz.QuadPart > 0 && sz.QuadPart <= 12000000 && (r = control_target()) != NULL) {
		r->body = malloc((size_t)sz.QuadPart);
		DWORD got = 0;
		if (r->body && ReadFile(f, r->body, (DWORD)sz.QuadPart, &got, NULL) && got == sz.QuadPart) {
			r->len = got;
			snprintf(r->path, sizeof(r->path), "/api/background");
			control_start(r);
			r = NULL;
		}
	} else if (sz.QuadPart > 12000000) {
		SetWindowTextW(app.status, L"Картинка больше 12 МБ, выберите поменьше");
	}
	if (r) {
		free(r->body);
		free(r);
	}
	CloseHandle(f);
}

static void send_control(const char *fmt, ...)
{
	struct control_req *r = control_target();
	if (!r)
		return;
	char q[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(q, sizeof(q), fmt, ap);
	va_end(ap);
	snprintf(r->path, sizeof(r->path), "/api/set?%s", q);
	control_start(r);
}

static DWORD WINAPI discover_main(void *arg)
{
	(void)arg;
	struct found_device f[32];
	int n = discovery_run(1200, f, 32);
	EnterCriticalSection(&app.lock);
	memcpy(app.found, f, sizeof(f[0]) * n);
	app.nfound = n;
	LeaveCriticalSection(&app.lock);
	PostMessageW(app.wnd, WM_APP_FOUND, 0, 0);
	return 0;
}

static DWORD WINAPI install_main(void *arg)
{
	(void)arg;
	wchar_t dir[MAX_PATH], script[MAX_PATH], params[MAX_PATH * 3];
	GetModuleFileNameW(NULL, dir, MAX_PATH);
	wchar_t *slash = wcsrchr(dir, L'\\');
	if (slash)
		*slash = 0;
	_snwprintf(script, MAX_PATH, L"%ls\\install.ps1", dir);
	_snwprintf(params, MAX_PATH * 3, L"-NoProfile -ExecutionPolicy Bypass -File \"%ls\" -Src \"%ls\"", script, dir);
	SHELLEXECUTEINFOW sei = {sizeof(sei)};
	sei.fMask = SEE_MASK_NOCLOSEPROCESS;
	sei.lpVerb = L"runas";
	sei.lpFile = L"powershell.exe";
	sei.lpParameters = params;
	sei.nShow = SW_HIDE;
	DWORD code = 1;
	if (ShellExecuteExW(&sei) && sei.hProcess) {
		WaitForSingleObject(sei.hProcess, INFINITE);
		GetExitCodeProcess(sei.hProcess, &code);
		CloseHandle(sei.hProcess);
	}
	PostMessageW(app.wnd, WM_APP_INSTALLED, code, 0);
	return 0;
}

static HWND mk(const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), app.wnd,
				 (HMENU)(INT_PTR)id, app.inst, NULL);
	SendMessageW(c, WM_SETFONT, (WPARAM)app.font, FALSE);
	return c;
}

static HWND label(const wchar_t *text, int x, int y, int w, int h)
{
	return mk(L"STATIC", text, SS_LEFT, x, y, w, h, -1);
}

static HWND group(const wchar_t *text, int x, int y, int w, int h)
{
	return mk(L"BUTTON", text, BS_GROUPBOX, x, y, w, h, -1);
}

static void combo_add(HWND cb, const wchar_t *text, LPARAM data)
{
	int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)text);
	SendMessageW(cb, CB_SETITEMDATA, i, data);
}

static void combo_select_data(HWND cb, LPARAM data)
{
	int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
	for (int i = 0; i < n; i++)
		if (SendMessageW(cb, CB_GETITEMDATA, i, 0) == data) {
			SendMessageW(cb, CB_SETCURSEL, i, 0);
			return;
		}
}

static LPARAM combo_data(HWND cb)
{
	int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
	return i < 0 ? -1 : SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

static void update_connect_button(void)
{
	SetWindowTextW(app.connect, app.want_connect ? L"Отключить" : L"Подключить");
	EnableWindow(app.host, app.want_connect ? FALSE : TRUE);
}

static void update_vcam_label(void)
{
	const wchar_t *t;
	wchar_t buf[200];
	bool show_install = false;
	switch (app.vcam) {
	case VCAM_READY:
		_snwprintf(buf, 200,
			   app.vcam_in_use ? L"Виртуальная камера «%ls»: используется приложением ●"
					   : L"Виртуальная камера «%ls»: готова. Выберите её в Zoom, Discord, Teams, браузере…",
			   app.cam_name);
		t = buf;
		break;
	case VCAM_NOT_INSTALLED:
		t = L"Виртуальная камера не установлена.";
		show_install = true;
		break;
	case VCAM_UNSUPPORTED:
		t = L"Виртуальная камера требует Windows 11. Используйте OBS: «Запустить виртуальную камеру».";
		break;
	case VCAM_FAILED:
		_snwprintf(buf, 200, L"Не удалось создать виртуальную камеру (0x%08lX).", (unsigned long)app.vcam_hr);
		t = buf;
		show_install = true;
		break;
	default:
		t = L"Виртуальная камера выключена.";
	}
	SetWindowTextW(app.vcam_label, t);
	ShowWindow(app.install_btn, show_install ? SW_SHOW : SW_HIDE);
}

static void fill_audio_devices(void)
{
	app.ndevs = audio_list_devices(app.devs, AUDIO_MAX_DEVICES);
	SendMessageW(app.audio_dev, CB_RESETCONTENT, 0, 0);
	int sel = -1, cable = -1;
	for (int i = 0; i < app.ndevs; i++) {
		combo_add(app.audio_dev, app.devs[i].name, i);
		if (!wcscmp(app.devs[i].id, app.cfg.audio_dev))
			sel = i;
		if (cable < 0 && wcsstr(app.devs[i].name, L"CABLE Input"))
			cable = i;
	}
	if (sel < 0)
		sel = cable;
	if (sel >= 0) {
		SendMessageW(app.audio_dev, CB_SETCURSEL, sel, 0);
		EnterCriticalSection(&app.lock);
		wcscpy(app.cfg.audio_dev, app.devs[sel].id);
		LeaveCriticalSection(&app.lock);
	}
}

static double zoom_from_pos(int pos, double zmin, double zmax)
{
	if (zmax <= zmin)
		return zmin;
	return zmin * pow(zmax / zmin, pos / 100.0);
}

static int pos_from_zoom(double z, double zmin, double zmax)
{
	if (zmax <= zmin || z <= 0)
		return 0;
	return (int)lround(100.0 * log(z / zmin) / log(zmax / zmin));
}

static void sync_phone_controls(void)
{
	EnterCriticalSection(&app.lock);
	struct phone_status ph = app.ph;
	char cur_cam[64];
	snprintf(cur_cam, sizeof(cur_cam), "%s", app.cfg.camera);
	LeaveCriticalSection(&app.lock);
	app.updating_ui = true;

	SendMessageW(app.camera, CB_RESETCONTENT, 0, 0);
	if (ph.valid && ph.ncams > 0) {
		int sel = -1;
		for (int i = 0; i < ph.ncams; i++) {
			combo_add(app.camera, ph.cams[i].label, i);
			if (!strcmp(ph.cams[i].id, ph.camera))
				sel = i;
		}
		SendMessageW(app.camera, CB_SETCURSEL, sel, 0);
	} else {
		combo_add(app.camera, L"Задняя (основная)", 1000);
		combo_add(app.camera, L"Фронтальная", 1001);
		SendMessageW(app.camera, CB_SETCURSEL, strcmp(cur_cam, "front") == 0 ? 1 : 0, 0);
	}

	bool on = ph.valid;
	EnableWindow(app.torch, on && ph.has_flash);
	SetWindowTextW(app.torch, ph.torch ? L"Фонарик: вкл" : L"Фонарик");
	EnableWindow(app.zoom, on && ph.zoom_max > ph.zoom_min);
	EnableWindow(app.ev, on && ph.ev_max > ph.ev_min);
	if (on && !app.zoom_dirty)
		SendMessageW(app.zoom, TBM_SETPOS, TRUE, pos_from_zoom(ph.zoom, ph.zoom_min, ph.zoom_max));
	if (on && !app.ev_dirty) {
		SendMessageW(app.ev, TBM_SETRANGE, TRUE, MAKELPARAM((int)lround(ph.ev_min * 10), (int)lround(ph.ev_max * 10)));
		SendMessageW(app.ev, TBM_SETPOS, TRUE, (int)lround(ph.ev * 10));
	}
	wchar_t b[64];
	_snwprintf(b, 64, L"Зум: %.1fx", on ? ph.zoom : 1.0);
	SetWindowTextW(app.zoom_label, b);
	_snwprintf(b, 64, L"Экспозиция: %+.1f", on ? ph.ev : 0.0);
	SetWindowTextW(app.ev_label, b);

	HWND phone_ctl[] = {app.profile, app.bg, app.framing, app.lock_btn, app.abr, app.heat, app.rec};
	for (size_t i = 0; i < sizeof(phone_ctl) / sizeof(phone_ctl[0]); i++)
		EnableWindow(phone_ctl[i], on);
	EnableWindow(app.bg_strength, on && strcmp(ph.bg, "blur") == 0);
	if (on) {
		static const char *profiles[] = {"", "call", "stream", "doc"};
		int pi = 0;
		for (int i = 0; i < 4; i++)
			if (!strcmp(ph.profile, profiles[i]))
				pi = i;
		SendMessageW(app.profile, CB_SETCURSEL, pi, 0);
		SendMessageW(app.bg, CB_SETCURSEL, !strcmp(ph.bg, "blur") ? 1 : !strcmp(ph.bg, "image") ? 2 : 0, 0);
		if (!app.bg_dirty)
			SendMessageW(app.bg_strength, TBM_SETPOS, TRUE, (int)lround(ph.bg_strength * 100));
		Button_SetCheck(app.framing, ph.framing);
		Button_SetCheck(app.lock_btn, ph.lock);
		Button_SetCheck(app.abr, ph.abr);
		Button_SetCheck(app.heat, ph.heat_guard);
		SetWindowTextW(app.rec, ph.rec ? L"■ Стоп запись" : L"● Записать видео");
	}
	wchar_t info[256] = L"";
	if (on) {
		wchar_t *o = info;
		size_t left = 256;
		int k;
#define ADD(...)                                          \
	do {                                              \
		k = _snwprintf(o, left, __VA_ARGS__);     \
		if (k > 0 && (size_t)k < left) {          \
			o += k;                           \
			left -= (size_t)k;                \
		}                                         \
	} while (0)
		if (ph.rec)
			ADD(L"● Запись %02lld:%02lld  ", ph.rec_ms / 60000, ph.rec_ms / 1000 % 60);
		if (ph.hot)
			ADD(L"Телефон перегрет, качество снижено  ");
		if (ph.framing && !ph.framing_available)
			ADD(L"Эта камера не умеет искать лица  ");
		else if (ph.framing && !ph.faces)
			ADD(L"Лицо не найдено  ");
		if (ph.bg_error[0])
			ADD(L"Фон: %ls  ", ph.bg_error);
		if (ph.rec_error[0])
			ADD(L"Запись: %ls", ph.rec_error);
#undef ADD
	}
	SetWindowTextW(app.phone_info, info);
	app.updating_ui = false;
}

static void set_track_label(HWND tb, const wchar_t *name)
{
	wchar_t b[64];
	_snwprintf(b, 64, L"%ls: %+d", name, (int)SendMessageW(tb, TBM_GETPOS, 0, 0));
	SetWindowTextW(GetWindow(tb, GW_HWNDPREV), b);
}

static void sync_color_controls(void)
{
	struct color_params p = app.cfg.color;
	SendMessageW(app.bright, TBM_SETPOS, TRUE, p.brightness);
	SendMessageW(app.contrast, TBM_SETPOS, TRUE, p.contrast);
	SendMessageW(app.sat, TBM_SETPOS, TRUE, p.saturation);
	SendMessageW(app.warm, TBM_SETPOS, TRUE, p.warmth);
	set_track_label(app.bright, L"Яркость");
	set_track_label(app.contrast, L"Контраст");
	set_track_label(app.sat, L"Насыщенность");
	set_track_label(app.warm, L"Теплота");
	Button_SetCheck(app.doc, p.doc);
	const wchar_t *name = wcsrchr(app.cfg.lut_path, L'\\');
	wchar_t b[MAX_PATH + 16];
	if (app.lut && name)
		_snwprintf(b, MAX_PATH + 16, L"LUT: %ls", name + 1);
	else
		wcscpy(b, L"LUT (.cube)…");
	SetWindowTextW(app.lut_btn, b);
}

static void build_ui(void)
{
	NONCLIENTMETRICSW ncm = {sizeof(ncm)};
	SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
	ncm.lfMessageFont.lfHeight = -S(13);
	app.font = CreateFontIndirectW(&ncm.lfMessageFont);
	ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
	app.font_bold = CreateFontIndirectW(&ncm.lfMessageFont);

	app.preview = mk(L"STATIC", L"", SS_OWNERDRAW, 12, 12, PREVIEW_W, PREVIEW_H, ID_PREVIEW);
	app.status = label(L"", 12, 380, PREVIEW_W, 22);
	SendMessageW(app.status, WM_SETFONT, (WPARAM)app.font_bold, FALSE);

	group(L"Виртуальная камера", 12, 410, PREVIEW_W, 108);
	app.vcam_label = label(L"", 24, 430, PREVIEW_W - 24, 34);
	app.install_btn = mk(L"BUTTON", L"Установить виртуальную камеру…", BS_PUSHBUTTON, 24, 474, 250, 28, ID_INSTALL);
	app.autostart = mk(L"BUTTON", L"Запускать вместе с Windows", BS_AUTOCHECKBOX, 300, 468, 340, 20, ID_AUTOSTART);
	app.totray = mk(L"BUTTON", L"Сворачивать в трей (значок у часов)", BS_AUTOCHECKBOX, 300, 490, 340, 20, ID_TOTRAY);

	int x = 668, w = 300;
	group(L"Подключение", x, 6, w, 150);
	app.wifi = mk(L"BUTTON", L"Wi-Fi", BS_AUTORADIOBUTTON | WS_GROUP, x + 12, 28, 90, 22, ID_WIFI);
	app.usb = mk(L"BUTTON", L"USB (ADB)", BS_AUTORADIOBUTTON, x + 110, 28, 120, 22, ID_USB);
	label(L"IP телефона:", x + 12, 58, 90, 20);
	app.host = mk(L"COMBOBOX", L"", CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL | WS_TABSTOP, x + 100, 54, 120, 200, ID_HOST);
	mk(L"BUTTON", L"Найти", BS_PUSHBUTTON | WS_TABSTOP, x + 226, 53, 62, 26, ID_FIND);
	app.connect = mk(L"BUTTON", L"Подключить", BS_DEFPUSHBUTTON | WS_TABSTOP, x + 12, 88, w - 24, 32, ID_CONNECT);
	app.autoconnect = mk(L"BUTTON", L"Подключаться при запуске", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 126, w - 24, 22, ID_AUTOCONNECT);

	group(L"Камера телефона", x, 162, w, 222);
	app.camera = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 12, 184, w - 24, 300, ID_CAMERA);
	app.res = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 12, 216, 110, 200, ID_RES);
	app.fps = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 128, 216, 72, 200, ID_FPS);
	app.bitrate = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 206, 216, 82, 200, ID_BITRATE);
	app.torch = mk(L"BUTTON", L"Фонарик", BS_PUSHBUTTON | WS_TABSTOP, x + 12, 248, 135, 28, ID_TORCH);
	mk(L"BUTTON", L"Фокус", BS_PUSHBUTTON | WS_TABSTOP, x + 153, 248, 135, 28, ID_AF);
	app.zoom_label = label(L"Зум", x + 12, 284, 200, 18);
	app.zoom = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + 6, 302, w - 12, 28, ID_ZOOM);
	app.ev_label = label(L"Экспозиция", x + 12, 332, 200, 18);
	app.ev = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + 6, 350, w - 12, 28, ID_EV);
	SendMessageW(app.zoom, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));

	group(L"Изображение", x, 390, w, 84);
	label(L"Поворот:", x + 12, 414, 70, 20);
	app.rot = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 80, 410, 208, 200, ID_ROT);
	app.mirror = mk(L"BUTTON", L"Зеркально", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 444, 110, 22, ID_MIRROR);
	app.fill = mk(L"BUTTON", L"Без полей (обрезать)", BS_AUTOCHECKBOX | WS_TABSTOP, x + 124, 444, 164, 22, ID_FILL);

	group(L"Звук телефона", x, 480, w, 82);
	app.audio = mk(L"BUTTON", L"Выводить микрофон телефона в:", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 500, w - 24, 22, ID_AUDIO);
	app.audio_dev = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, x + 12, 526, w - 24, 300, ID_AUDIO_DEV);

	x = 980;
	group(L"Доступ", x, 6, w, 92);
	label(L"Код:", x + 12, 30, 40, 20);
	app.key = mk(L"EDIT", L"", WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL | WS_TABSTOP, x + 52, 26, 80, 24, ID_KEY);
	SendMessageW(app.key, EM_SETLIMITTEXT, 8, 0);
	mk(L"BUTTON", L"QR-сопряжение…", BS_PUSHBUTTON | WS_TABSTOP, x + 140, 24, 148, 28, ID_PAIR);
	app.hevc = mk(L"BUTTON", L"H.265 (меньше трафика)", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 62, 170, 22, ID_HEVC);
	mk(L"BUTTON", L"Ещё телефон…", BS_PUSHBUTTON | WS_TABSTOP, x + 186, 58, 102, 28, ID_MORE);

	group(L"Функции телефона", x, 104, w, 238);
	label(L"Профиль:", x + 12, 128, 70, 20);
	app.profile = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 84, 124, 204, 200, ID_PROFILE);
	label(L"Фон:", x + 12, 160, 70, 20);
	app.bg = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, x + 84, 156, 204, 200, ID_BG);
	app.bg_strength_label = label(L"Размытие:", x + 12, 192, 70, 20);
	app.bg_strength = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + 80, 188, 212, 28, ID_BG_STRENGTH);
	SendMessageW(app.bg_strength, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100));
	mk(L"BUTTON", L"Картинка фона…", BS_PUSHBUTTON | WS_TABSTOP, x + 12, 220, 135, 28, ID_BG_PICK);
	app.rec = mk(L"BUTTON", L"● Записать видео", BS_PUSHBUTTON | WS_TABSTOP, x + 153, 220, 135, 28, ID_REC);
	app.framing = mk(L"BUTTON", L"Следить за лицом", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 254, 140, 22, ID_FRAMING);
	app.lock_btn = mk(L"BUTTON", L"Фикс. экспозиции", BS_AUTOCHECKBOX | WS_TABSTOP, x + 156, 254, 132, 22, ID_LOCK);
	app.abr = mk(L"BUTTON", L"Адаптивный битрейт", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 278, 140, 22, ID_ABR);
	app.heat = mk(L"BUTTON", L"Защита от нагрева", BS_AUTOCHECKBOX | WS_TABSTOP, x + 156, 278, 132, 22, ID_HEAT);
	app.phone_info = label(L"", x + 12, 304, w - 24, 34);

	group(L"Цвет на компьютере", x, 348, w, 214);
	static const struct { const wchar_t *name; int id; HWND *h; } sliders[] = {
		{L"Яркость", ID_BRIGHT, &app.bright},
		{L"Контраст", ID_CONTRAST, &app.contrast},
		{L"Насыщенность", ID_SAT, &app.sat},
		{L"Теплота", ID_WARM, &app.warm},
	};
	for (int i = 0; i < 4; i++) {
		/* подпись создаю прямо перед ползунком: set_track_label находит её через GW_HWNDPREV */
		label(sliders[i].name, x + 12, 372 + i * 30, 110, 20);
		*sliders[i].h = mk(TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, x + 120, 368 + i * 30, 172, 26,
				   sliders[i].id);
		SendMessageW(*sliders[i].h, TBM_SETRANGE, TRUE, MAKELPARAM(-100, 100));
	}
	app.doc = mk(L"BUTTON", L"Режим документа (чёткий текст)", BS_AUTOCHECKBOX | WS_TABSTOP, x + 12, 492,
		     w - 24, 22, ID_DOC);
	app.lut_btn = mk(L"BUTTON", L"LUT (.cube)…", BS_PUSHBUTTON | WS_TABSTOP, x + 12, 522, 180, 28, ID_LUT);
	mk(L"BUTTON", L"Сбросить", BS_PUSHBUTTON | WS_TABSTOP, x + 198, 522, 90, 28, ID_COLOR_RESET);

	combo_add(app.profile, L"Свои настройки", 0);
	combo_add(app.profile, L"Звонок (720p, лицо)", 1);
	combo_add(app.profile, L"Стрим (1080p60)", 2);
	combo_add(app.profile, L"Документ", 3);
	combo_add(app.bg, L"Без обработки", 0);
	combo_add(app.bg, L"Размыть фон", 1);
	combo_add(app.bg, L"Заменить картинкой", 2);
	wchar_t *kw = utf8_to_wide(app.cfg.key);
	SetWindowTextW(app.key, kw);
	free(kw);
	Button_SetCheck(app.hevc, app.cfg.hevc);
	if (!g_hevc_decoder) {
		EnableWindow(app.hevc, FALSE);
		SetWindowTextW(app.hevc, L"H.265 (нет декодера)");
	}
	sync_color_controls();

	HWND hint = label(L"Микрофон для Zoom/Discord: установите VB-Audio Virtual Cable, выберите здесь «CABLE Input», "
			  L"а в приложении — микрофон «CABLE Output».",
			  12, 524, PREVIEW_W, 40);
	(void)hint;

	static const struct { const wchar_t *t; int w, h; } res[] = {
		{L"640×480", 640, 480}, {L"1280×720", 1280, 720}, {L"1920×1080", 1920, 1080}};
	for (int i = 0; i < 3; i++)
		combo_add(app.res, res[i].t, MAKELPARAM(res[i].w / 8, res[i].h / 8));
	combo_select_data(app.res, MAKELPARAM(app.cfg.width / 8, app.cfg.height / 8));
	if (SendMessageW(app.res, CB_GETCURSEL, 0, 0) < 0)
		SendMessageW(app.res, CB_SETCURSEL, 2, 0);
	static const int fps[] = {15, 24, 30, 60};
	for (int i = 0; i < 4; i++) {
		wchar_t b[16];
		_snwprintf(b, 16, L"%d к/с", fps[i]);
		combo_add(app.fps, b, fps[i]);
	}
	combo_select_data(app.fps, app.cfg.fps);
	static const int br[] = {2000, 4000, 8000, 12000, 20000};
	for (int i = 0; i < 5; i++) {
		wchar_t b[24];
		_snwprintf(b, 24, L"%d Мбит/с", br[i] / 1000);
		combo_add(app.bitrate, b, br[i]);
	}
	combo_select_data(app.bitrate, app.cfg.bitrate_kbps);
	combo_add(app.rot, L"Авто (по телефону)", -1);
	combo_add(app.rot, L"0°", 0);
	combo_add(app.rot, L"90°", 90);
	combo_add(app.rot, L"180°", 180);
	combo_add(app.rot, L"270°", 270);
	combo_select_data(app.rot, app.cfg.rotation);

	SendMessageW(app.cfg.conn == 1 ? app.usb : app.wifi, BM_SETCHECK, BST_CHECKED, 0);
	wchar_t *h = utf8_to_wide(app.cfg.host);
	SetWindowTextW(app.host, h);
	free(h);
	Button_SetCheck(app.autoconnect, app.cfg.autoconnect);
	Button_SetCheck(app.mirror, app.cfg.mirror);
	Button_SetCheck(app.fill, app.cfg.fill);
	Button_SetCheck(app.audio, app.cfg.audio);
	Button_SetCheck(app.autostart, autostart_enabled());
	Button_SetCheck(app.totray, app.cfg.to_tray);
	EnableWindow(app.host, app.cfg.conn == 0);
	fill_audio_devices();
	sync_phone_controls();
	update_connect_button();
	update_vcam_label();
}

static void read_host_field(void)
{
	wchar_t w[128];
	GetWindowTextW(app.host, w, 128);
	/* из списка поиска приходит «Имя — 192.168.1.5», мне нужен только адрес */
	wchar_t *dash = wcsrchr(w, L' ');
	const wchar_t *ip = dash ? dash + 1 : w;
	EnterCriticalSection(&app.lock);
	wide_to_utf8(ip, app.cfg.host, sizeof(app.cfg.host));
	LeaveCriticalSection(&app.lock);
}

static void tray_add(void)
{
	app.nid.cbSize = sizeof(app.nid);
	app.nid.hWnd = app.wnd;
	app.nid.uID = 1;
	app.nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
	app.nid.uCallbackMessage = WM_APP_TRAY;
	app.nid.hIcon = LoadIconW(app.inst, MAKEINTRESOURCEW(1));
	if (!app.nid.szTip[0])
		wcscpy(app.nid.szTip, L"MagerCam");
	Shell_NotifyIconW(NIM_ADD, &app.nid);
}

static void tray_tip(const wchar_t *status)
{
	_snwprintf(app.nid.szTip, 127, L"%ls: %ls", app.cam_name, status);
	app.nid.szTip[127] = 0;
	app.nid.uFlags = NIF_TIP;
	Shell_NotifyIconW(NIM_MODIFY, &app.nid);
}

static void hide_to_tray(void)
{
	ShowWindow(app.wnd, SW_HIDE);
	if (!app.tray_hint_shown) {
		app.tray_hint_shown = true;
		app.nid.uFlags = NIF_INFO;
		wcscpy(app.nid.szInfoTitle, L"MagerCam работает в фоне");
		wcscpy(app.nid.szInfo, L"Виртуальная камера остаётся доступной. Открыть или выйти — через этот значок.");
		app.nid.dwInfoFlags = NIIF_INFO;
		Shell_NotifyIconW(NIM_MODIFY, &app.nid);
	}
}

static void show_window(void)
{
	ShowWindow(app.wnd, SW_SHOW);
	ShowWindow(app.wnd, SW_RESTORE);
	SetForegroundWindow(app.wnd);
}

static void tray_menu(void)
{
	HMENU m = CreatePopupMenu();
	AppendMenuW(m, MF_STRING, ID_TRAY_OPEN, L"Открыть MagerCam");
	AppendMenuW(m, MF_STRING, ID_TRAY_CONNECT, app.want_connect ? L"Отключить телефон" : L"Подключить телефон");
	AppendMenuW(m, MF_SEPARATOR, 0, NULL);
	AppendMenuW(m, MF_STRING, ID_TRAY_EXIT, L"Выход");
	SetMenuDefaultItem(m, ID_TRAY_OPEN, FALSE);
	POINT pt;
	GetCursorPos(&pt);
	SetForegroundWindow(app.wnd);
	TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, app.wnd, NULL);
	PostMessageW(app.wnd, WM_NULL, 0, 0);
	DestroyMenu(m);
}

static void on_command(int id, int code)
{
	if (app.updating_ui)
		return;
	switch (id) {
	case ID_WIFI:
	case ID_USB:
		if (code == BN_CLICKED) {
			EnterCriticalSection(&app.lock);
			app.cfg.conn = id == ID_USB;
			LeaveCriticalSection(&app.lock);
			EnableWindow(app.host, id == ID_WIFI && !app.want_connect);
			save_settings();
			if (app.want_connect)
				reconnect();
		}
		break;
	case ID_FIND:
		SetWindowTextW(app.status, L"Поиск телефонов в сети…");
		CloseHandle(CreateThread(NULL, 0, discover_main, NULL, 0, NULL));
		break;
	case ID_CONNECT:
		if (!app.want_connect)
			read_host_field();
		app.want_connect = !app.want_connect;
		save_settings();
		update_connect_button();
		if (!app.want_connect)
			set_net_status(L"Отключено");
		reconnect();
		break;
	case ID_AUTOCONNECT:
		EnterCriticalSection(&app.lock);
		app.cfg.autoconnect = Button_GetCheck(app.autoconnect) == BST_CHECKED;
		LeaveCriticalSection(&app.lock);
		save_settings();
		break;
	case ID_TOTRAY:
		EnterCriticalSection(&app.lock);
		app.cfg.to_tray = Button_GetCheck(app.totray) == BST_CHECKED;
		LeaveCriticalSection(&app.lock);
		save_settings();
		break;
	case ID_TRAY_OPEN:
		show_window();
		break;
	case ID_TRAY_CONNECT:
		on_command(ID_CONNECT, BN_CLICKED);
		break;
	case ID_TRAY_EXIT:
		DestroyWindow(app.wnd);
		break;
	case ID_AUTOSTART:
		set_autostart(Button_GetCheck(app.autostart) == BST_CHECKED);
		break;
	case ID_CAMERA:
		if (code == CBN_SELCHANGE) {
			int i = (int)combo_data(app.camera);
			char id_str[64] = "";
			EnterCriticalSection(&app.lock);
			if (i >= 1000)
				snprintf(id_str, sizeof(id_str), "%s", i == 1001 ? "front" : "back");
			else if (i >= 0 && i < app.ph.ncams)
				snprintf(id_str, sizeof(id_str), "%s", app.ph.cams[i].id);
			if (id_str[0])
				snprintf(app.cfg.camera, sizeof(app.cfg.camera), "%s", id_str);
			bool conn = app.connected;
			LeaveCriticalSection(&app.lock);
			save_settings();
			if (id_str[0] && conn) {
				char enc[200];
				url_encode(enc, sizeof(enc), id_str);
				send_control("camera=%s", enc);
			}
		}
		break;
	case ID_RES:
	case ID_FPS:
	case ID_BITRATE:
		if (code == CBN_SELCHANGE) {
			LPARAM r = combo_data(app.res);
			EnterCriticalSection(&app.lock);
			app.cfg.width = LOWORD(r) * 8;
			app.cfg.height = HIWORD(r) * 8;
			app.cfg.fps = (int)combo_data(app.fps);
			app.cfg.bitrate_kbps = (int)combo_data(app.bitrate);
			struct settings c = app.cfg;
			LeaveCriticalSection(&app.lock);
			save_settings();
			send_control("w=%d&h=%d&fps=%d&bitrate=%d", c.width, c.height, c.fps, c.bitrate_kbps);
		}
		break;
	case ID_TORCH:
		send_control("torch=toggle");
		break;
	case ID_AF:
		send_control("af=trigger&x=0.5&y=0.5");
		break;
	case ID_ROT:
		if (code == CBN_SELCHANGE) {
			EnterCriticalSection(&app.lock);
			app.cfg.rotation = (int)combo_data(app.rot);
			LeaveCriticalSection(&app.lock);
			save_settings();
		}
		break;
	case ID_MIRROR:
	case ID_FILL:
		EnterCriticalSection(&app.lock);
		app.cfg.mirror = Button_GetCheck(app.mirror) == BST_CHECKED;
		app.cfg.fill = Button_GetCheck(app.fill) == BST_CHECKED;
		LeaveCriticalSection(&app.lock);
		save_settings();
		break;
	case ID_AUDIO: {
		bool on = Button_GetCheck(app.audio) == BST_CHECKED;
		EnterCriticalSection(&app.lock);
		app.cfg.audio = on;
		LeaveCriticalSection(&app.lock);
		save_settings();
		if (app.want_connect)
			reconnect(); /* звук телефон шлёт, только если попросить его при подключении */
		break;
	}
	case ID_AUDIO_DEV:
		if (code == CBN_DROPDOWN) {
			fill_audio_devices();
		} else if (code == CBN_SELCHANGE) {
			int i = (int)combo_data(app.audio_dev);
			if (i >= 0 && i < app.ndevs) {
				EnterCriticalSection(&app.lock);
				wcscpy(app.cfg.audio_dev, app.devs[i].id);
				LeaveCriticalSection(&app.lock);
				save_settings();
			}
		}
		break;
	case ID_INSTALL:
		EnableWindow(app.install_btn, FALSE);
		SetWindowTextW(app.vcam_label, L"Установка… подтвердите запрос Windows (UAC).");
		CloseHandle(CreateThread(NULL, 0, install_main, NULL, 0, NULL));
		break;
	case ID_KEY:
		if (code == EN_CHANGE) {
			wchar_t k[16];
			GetWindowTextW(app.key, k, 16);
			EnterCriticalSection(&app.lock);
			wide_to_utf8(k, app.cfg.key, sizeof(app.cfg.key));
			LeaveCriticalSection(&app.lock);
			save_settings();
			/* переподключаюсь только на полном коде, иначе каждая цифра — неудачная попытка */
			size_t n = wcslen(k);
			if (app.want_connect && (n == 6 || n == 0))
				reconnect();
		}
		break;
	case ID_PAIR:
		if (!pair_show(app.wnd, app.inst, app.font, app.dpi, WM_APP_PAIRED))
			SetWindowTextW(app.status, L"Не удалось открыть порт для сопряжения (4850–4859 заняты?)");
		break;
	case ID_HEVC:
		EnterCriticalSection(&app.lock);
		app.cfg.hevc = Button_GetCheck(app.hevc) == BST_CHECKED;
		LeaveCriticalSection(&app.lock);
		save_settings();
		if (app.want_connect)
			reconnect(); /* кодек выбирается при подключении */
		break;
	case ID_MORE: {
		/* следующая свободная камера: «MagerCam 2», «MagerCam 3»… каждая в своём окне */
		int free_slot = -1;
		for (int i = 0; i < MCV_MAX_SLOTS && free_slot < 0; i++) {
			wchar_t cls[48];
			_snwprintf(cls, 48, i ? L"MagerCamClient%d" : L"MagerCamClient", i + 1);
			if (i != app.slot && !FindWindowW(cls, NULL))
				free_slot = i;
		}
		if (free_slot < 0) {
			SetWindowTextW(app.status, L"Уже открыты все четыре камеры MagerCam");
			break;
		}
		wchar_t exe[MAX_PATH], args[32];
		GetModuleFileNameW(NULL, exe, MAX_PATH);
		_snwprintf(args, 32, L"--slot %d", free_slot + 1);
		ShellExecuteW(NULL, L"open", exe, args, NULL, SW_SHOWNORMAL);
		break;
	}
	case ID_PROFILE:
		if (code == CBN_SELCHANGE) {
			static const char *profiles[] = {"", "call", "stream", "doc"};
			int i = (int)combo_data(app.profile);
			if (i > 0 && i < 4)
				send_control("profile=%s", profiles[i]);
		}
		break;
	case ID_BG:
		if (code == CBN_SELCHANGE) {
			static const char *modes[] = {"off", "blur", "image"};
			int i = (int)combo_data(app.bg);
			EnterCriticalSection(&app.lock);
			bool have_image = app.ph.bg_image;
			LeaveCriticalSection(&app.lock);
			if (i == 2 && !have_image)
				on_command(ID_BG_PICK, BN_CLICKED); /* картинки на телефоне ещё нет — сначала выбрать */
			else if (i >= 0 && i < 3)
				send_control("bg=%s", modes[i]);
		}
		break;
	case ID_BG_PICK: {
		wchar_t path[MAX_PATH] = L"";
		OPENFILENAMEW ofn = {sizeof(ofn)};
		ofn.hwndOwner = app.wnd;
		ofn.lpstrFilter = L"Изображения (*.jpg, *.png)\0*.jpg;*.jpeg;*.png\0Все файлы\0*.*\0";
		ofn.lpstrFile = path;
		ofn.nMaxFile = MAX_PATH;
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
		if (GetOpenFileNameW(&ofn)) {
			SetWindowTextW(app.status, L"Отправляю картинку фона на телефон…");
			send_background(path);
		} else {
			sync_phone_controls();
		}
		break;
	}
	case ID_FRAMING:
		send_control("framing=%d", Button_GetCheck(app.framing) == BST_CHECKED);
		break;
	case ID_LOCK:
		send_control("lock=%d", Button_GetCheck(app.lock_btn) == BST_CHECKED);
		break;
	case ID_ABR:
		send_control("abr=%d", Button_GetCheck(app.abr) == BST_CHECKED);
		break;
	case ID_HEAT:
		send_control("heat_guard=%d", Button_GetCheck(app.heat) == BST_CHECKED);
		break;
	case ID_REC: {
		EnterCriticalSection(&app.lock);
		bool rec = app.ph.rec;
		LeaveCriticalSection(&app.lock);
		send_control("rec=%d", !rec);
		break;
	}
	case ID_DOC:
		EnterCriticalSection(&app.lock);
		app.cfg.color.doc = Button_GetCheck(app.doc) == BST_CHECKED;
		LeaveCriticalSection(&app.lock);
		save_settings();
		rebuild_color();
		break;
	case ID_LUT: {
		wchar_t path[MAX_PATH] = L"";
		OPENFILENAMEW ofn = {sizeof(ofn)};
		ofn.hwndOwner = app.wnd;
		ofn.lpstrFilter = L"3D LUT (*.cube)\0*.cube\0";
		ofn.lpstrFile = path;
		ofn.nMaxFile = MAX_PATH;
		ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY;
		if (!GetOpenFileNameW(&ofn))
			break;
		char err[160];
		if (!load_lut(path, err, sizeof(err))) {
			wchar_t *we = utf8_to_wide(err), b[256];
			_snwprintf(b, 256, L"LUT не загружен: %ls", we);
			SetWindowTextW(app.status, b);
			free(we);
			break;
		}
		EnterCriticalSection(&app.lock);
		wcscpy(app.cfg.lut_path, path);
		LeaveCriticalSection(&app.lock);
		save_settings();
		rebuild_color();
		sync_color_controls();
		break;
	}
	case ID_COLOR_RESET:
		EnterCriticalSection(&app.lock);
		memset(&app.cfg.color, 0, sizeof(app.cfg.color));
		app.cfg.lut_path[0] = 0;
		LeaveCriticalSection(&app.lock);
		load_lut(NULL, NULL, 0);
		save_settings();
		rebuild_color();
		sync_color_controls();
		break;
	}
}

static void paint_preview(const DRAWITEMSTRUCT *di)
{
	RECT r = di->rcItem;
	HDC dc = di->hDC;
	EnterCriticalSection(&app.lock);
	bool valid = app.preview_valid;
	if (valid) {
		BITMAPINFO bi = {0};
		bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
		bi.bmiHeader.biWidth = PREVIEW_W;
		bi.bmiHeader.biHeight = -PREVIEW_H;
		bi.bmiHeader.biPlanes = 1;
		bi.bmiHeader.biBitCount = 32;
		SetStretchBltMode(dc, HALFTONE);
		StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, PREVIEW_W, PREVIEW_H,
			      app.preview_bgra, &bi, DIB_RGB_COLORS, SRCCOPY);
	}
	LeaveCriticalSection(&app.lock);
	if (!valid) {
		HBRUSH b = CreateSolidBrush(RGB(24, 26, 32));
		FillRect(dc, &r, b);
		DeleteObject(b);
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(150, 156, 170));
		HFONT old = SelectObject(dc, app.font_bold);
		DrawTextW(dc, app.want_connect ? L"Ожидание видео с телефона…" : L"Телефон не подключён", -1, &r,
			  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		SelectObject(dc, old);
	}
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		app.wnd = w;
		build_ui();
		app.taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
		tray_add();
		SetTimer(w, TIMER_VCAM, 1000, NULL);
		return 0;
	case WM_CLOSE:
		if (app.cfg.to_tray) {
			hide_to_tray();
			return 0;
		}
		break;
	case WM_SIZE:
		if (wp == SIZE_MINIMIZED && app.cfg.to_tray)
			hide_to_tray();
		return 0;
	case WM_APP_TRAY:
		if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
			show_window();
		else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU)
			tray_menu();
		return 0;
	case WM_COMMAND:
		on_command(LOWORD(wp), HIWORD(wp));
		return 0;
	case WM_DRAWITEM:
		if (wp == ID_PREVIEW) {
			paint_preview((const DRAWITEMSTRUCT *)lp);
			return TRUE;
		}
		break;
	case WM_CTLCOLORSTATIC: {
		HDC dc = (HDC)wp;
		SetBkMode(dc, TRANSPARENT);
		return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
	}
	case WM_HSCROLL: {
		HWND tb = (HWND)lp;
		if (tb == app.bright || tb == app.contrast || tb == app.sat || tb == app.warm) {
			EnterCriticalSection(&app.lock);
			app.cfg.color.brightness = (int)SendMessageW(app.bright, TBM_GETPOS, 0, 0);
			app.cfg.color.contrast = (int)SendMessageW(app.contrast, TBM_GETPOS, 0, 0);
			app.cfg.color.saturation = (int)SendMessageW(app.sat, TBM_GETPOS, 0, 0);
			app.cfg.color.warmth = (int)SendMessageW(app.warm, TBM_GETPOS, 0, 0);
			LeaveCriticalSection(&app.lock);
			set_track_label(app.bright, L"Яркость");
			set_track_label(app.contrast, L"Контраст");
			set_track_label(app.sat, L"Насыщенность");
			set_track_label(app.warm, L"Теплота");
			SetTimer(w, TIMER_COLOR, 80, NULL); /* таблицу собираю, когда ползунок замер */
			return 0;
		}
		if (tb == app.zoom)
			app.zoom_dirty = true;
		else if (tb == app.ev)
			app.ev_dirty = true;
		else if (tb == app.bg_strength)
			app.bg_dirty = true;
		SetTimer(w, TIMER_CONTROLS, 120, NULL);
		return 0;
	}
	case WM_TIMER:
		if (wp == TIMER_CONTROLS) {
			KillTimer(w, TIMER_CONTROLS);
			EnterCriticalSection(&app.lock);
			struct phone_status ph = app.ph;
			LeaveCriticalSection(&app.lock);
			if (app.zoom_dirty && ph.valid) {
				double z = zoom_from_pos((int)SendMessageW(app.zoom, TBM_GETPOS, 0, 0), ph.zoom_min, ph.zoom_max);
				send_control("zoom=%.2f", z);
				wchar_t b[64];
				_snwprintf(b, 64, L"Зум: %.1fx", z);
				SetWindowTextW(app.zoom_label, b);
			}
			if (app.ev_dirty && ph.valid) {
				double ev = (int)SendMessageW(app.ev, TBM_GETPOS, 0, 0) / 10.0;
				send_control("ev=%.1f", ev);
				wchar_t b[64];
				_snwprintf(b, 64, L"Экспозиция: %+.1f", ev);
				SetWindowTextW(app.ev_label, b);
			}
			if (app.bg_dirty && ph.valid)
				send_control("bg_strength=%.2f", (int)SendMessageW(app.bg_strength, TBM_GETPOS, 0, 0) / 100.0);
			/* даю телефону подтвердить значение, иначе старый статус дёрнет ползунки обратно */
			SetTimer(w, TIMER_CONTROLS + 10, 1500, NULL);
		} else if (wp == TIMER_CONTROLS + 10) {
			KillTimer(w, wp);
			app.zoom_dirty = app.ev_dirty = app.bg_dirty = false;
		} else if (wp == TIMER_COLOR) {
			KillTimer(w, wp);
			rebuild_color();
			save_settings();
		} else if (wp == TIMER_VCAM) {
			bool in_use = app.vcam == VCAM_READY && vcam_in_use();
			if (in_use != app.vcam_in_use) {
				app.vcam_in_use = in_use;
				update_vcam_label();
			}
		}
		return 0;
	case WM_APP_FRAME:
		InvalidateRect(app.preview, NULL, FALSE);
		return 0;
	case WM_APP_STATUS: {
		wchar_t s[256];
		EnterCriticalSection(&app.lock);
		wcscpy(s, app.net_status);
		LeaveCriticalSection(&app.lock);
		SetWindowTextW(app.status, s);
		tray_tip(s);
		if (wp == 1)
			sync_phone_controls();
		return 0;
	}
	case WM_APP_FOUND: {
		EnterCriticalSection(&app.lock);
		int n = app.nfound;
		struct found_device f[32];
		memcpy(f, app.found, sizeof(f[0]) * n);
		LeaveCriticalSection(&app.lock);
		SendMessageW(app.host, CB_RESETCONTENT, 0, 0);
		for (int i = 0; i < n; i++) {
			wchar_t *nm = utf8_to_wide(f[i].name), *ip = utf8_to_wide(f[i].ip), b[200];
			_snwprintf(b, 200, L"%ls — %ls", nm, ip);
			SendMessageW(app.host, CB_ADDSTRING, 0, (LPARAM)b);
			free(nm);
			free(ip);
		}
		if (n > 0) {
			SendMessageW(app.host, CB_SETCURSEL, 0, 0);
			SetWindowTextW(app.status, n == 1 ? L"Найден 1 телефон" : L"Найдены телефоны — выберите в списке");
			SendMessageW(app.host, CB_SHOWDROPDOWN, n > 1, 0);
		} else {
			SetWindowTextW(app.status, L"Телефоны не найдены. Впишите IP с экрана телефона.");
		}
		return 0;
	}
	case WM_APP_PAIRED: {
		struct pair_result *pr = (struct pair_result *)lp;
		pair_close();
		EnterCriticalSection(&app.lock);
		app.cfg.conn = 0;
		snprintf(app.cfg.host, sizeof(app.cfg.host), "%s", pr->ip);
		app.cfg.port = pr->port > 0 && pr->port < 65536 ? pr->port : MC_DEFAULT_PORT;
		snprintf(app.cfg.key, sizeof(app.cfg.key), "%s", pr->key);
		LeaveCriticalSection(&app.lock);
		app.updating_ui = true;
		Button_SetCheck(app.wifi, BST_CHECKED);
		Button_SetCheck(app.usb, BST_UNCHECKED);
		wchar_t *ip = utf8_to_wide(pr->ip), *key = utf8_to_wide(pr->key), *nm = utf8_to_wide(pr->name);
		SetWindowTextW(app.host, ip);
		SetWindowTextW(app.key, key);
		app.updating_ui = false;
		save_settings();
		app.want_connect = true;
		update_connect_button();
		set_net_status(L"Сопряжено с «%ls», подключаюсь…", nm);
		free(ip);
		free(key);
		free(nm);
		free(pr);
		reconnect();
		return 0;
	}
	case WM_APP_BG_SENT:
		SetWindowTextW(app.status, (int)wp == 200 ? L"Картинка фона отправлена на телефон"
				   : (int)wp == 415   ? L"Телефон не смог прочитать картинку (нужен JPEG или PNG)"
						      : L"Не удалось отправить картинку фона");
		if ((int)wp == 200)
			send_control("bg=image");
		return 0;
	case WM_APP_INSTALLED:
		EnableWindow(app.install_btn, TRUE);
		app.vcam = vcam_start(app.slot, &app.vcam_hr);
		update_vcam_label();
		if (app.vcam != VCAM_READY && wp != 0)
			SetWindowTextW(app.vcam_label, L"Установка не выполнена (отменена или ошибка). Лог: %TEMP%\\magercam-install.log");
		return 0;
	case WM_DESTROY:
		Shell_NotifyIconW(NIM_DELETE, &app.nid);
		PostQuitMessage(0);
		return 0;
	}
	if (msg == app.taskbar_created && msg) {
		tray_add(); /* Проводник перезапустился, иконку в трей добавляю заново */
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
	(void)prev;
	/* --slot N: N-я виртуальная камера для N-го телефона, у каждой своё окно и свои настройки */
	const wchar_t *slot_arg = cmdline ? wcsstr(cmdline, L"--slot") : NULL;
	if (slot_arg) {
		int n = _wtoi(slot_arg + 6);
		app.slot = n >= 1 && n <= MCV_MAX_SLOTS ? n - 1 : 0;
	}
	wchar_t mutex_name[48], cls[48];
	if (app.slot) {
		_snwprintf(app.section, 32, L"MagerCam%d", app.slot + 1);
		_snwprintf(app.cam_name, 32, L"MagerCam %d", app.slot + 1);
		_snwprintf(mutex_name, 48, L"Local\\MagerCamClient%d", app.slot + 1);
		_snwprintf(cls, 48, L"MagerCamClient%d", app.slot + 1);
	} else {
		wcscpy(app.section, L"MagerCam");
		wcscpy(app.cam_name, L"MagerCam");
		wcscpy(mutex_name, L"Local\\MagerCamClient");
		wcscpy(cls, L"MagerCamClient");
	}
	/* держу один экземпляр на камеру: виртуальную камеру и подключение к телефону не поделить между копиями */
	CreateMutexW(NULL, TRUE, mutex_name);
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		HWND other = FindWindowW(cls, NULL);
		if (other) {
			ShowWindow(other, SW_SHOW);
			ShowWindow(other, SW_RESTORE);
			SetForegroundWindow(other);
		}
		return 0;
	}
	bool minimized = cmdline && wcsstr(cmdline, L"--minimized");

	app.inst = inst;
	app.sock = INVALID_SOCKET;
	InitializeCriticalSection(&app.lock);
	app.preview_bgra = calloc(PREVIEW_W * PREVIEW_H, 4);
	app.wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
	MFStartup(MF_VERSION, MFSTARTUP_LITE);
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
	InitCommonControlsEx(&icc);
	load_settings();

	HDC sdc = GetDC(NULL);
	app.dpi = GetDeviceCaps(sdc, LOGPIXELSX);
	ReleaseDC(NULL, sdc);

	app.vcam = vcam_start(app.slot, &app.vcam_hr);
	g_hevc_decoder = decoder_hevc_available();
	char lut_err[160];
	if (app.cfg.lut_path[0] && !load_lut(app.cfg.lut_path, lut_err, sizeof(lut_err)))
		app.cfg.lut_path[0] = 0;
	rebuild_color();

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = cls;
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);
	RECT r = {0, 0, S(1292), S(572)};
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRect(&r, style, FALSE);
	wchar_t title[96];
	_snwprintf(title, 96, L"%ls — телефон как веб-камера", app.cam_name);
	HWND w = CreateWindowExW(0, wc.lpszClassName, title, style, CW_USEDEFAULT,
				 CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
	/* --minimized (автозапуск): только иконка в трее или свёрнутое окно без трея.
	 * Первый ShowWindow всё равно берёт режим показа от того, кто меня запустил,
	 * поэтому в случае трея окно просто ни разу не показываю. */
	if (!minimized) {
		ShowWindow(w, show);
		UpdateWindow(w);
	} else if (!app.cfg.to_tray) {
		ShowWindow(w, SW_SHOWMINNOACTIVE);
	}

	app.net_thread = CreateThread(NULL, 0, net_main, NULL, 0, NULL);
	if (app.cfg.autoconnect && (app.cfg.conn == 1 || app.cfg.host[0])) {
		app.want_connect = true;
		update_connect_button();
		SetEvent(app.wake);
	} else {
		set_net_status(L"Выберите способ подключения и нажмите «Подключить»");
	}

	MSG m;
	while (GetMessageW(&m, NULL, 0, 0) > 0) {
		if (!IsDialogMessageW(w, &m)) {
			TranslateMessage(&m);
			DispatchMessageW(&m);
		}
	}
	app.quit = true;
	reconnect();
	WaitForSingleObject(app.net_thread, 5000);
	save_settings();
	vcam_stop();
	MFShutdown();
	WSACleanup();
	return 0;
}
