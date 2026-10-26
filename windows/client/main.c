/*
 * MagerCam for Windows: connects to the phone (Wi-Fi or USB), shows a preview, feeds the
 * "MagerCam" virtual camera and optionally plays the phone microphone into a playback device.
 */
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
#include "../common/mcv_nv12.h"

#define APP_VERSION "1.0.0"
#define WM_APP_FRAME (WM_APP + 1)
#define WM_APP_STATUS (WM_APP + 2)
#define WM_APP_FOUND (WM_APP + 3)
#define WM_APP_INSTALLED (WM_APP + 4)
#define WM_APP_TRAY (WM_APP + 5)
#define TIMER_CONTROLS 1
#define TIMER_VCAM 2
#define PREVIEW_W 640
#define PREVIEW_H 360
#define MAX_CAMS 16

enum {
	ID_WIFI = 100, ID_USB, ID_HOST, ID_FIND, ID_CONNECT, ID_AUTOCONNECT,
	ID_CAMERA, ID_RES, ID_FPS, ID_BITRATE, ID_TORCH, ID_AF, ID_ZOOM, ID_EV, ID_ZOOM_LABEL, ID_EV_LABEL,
	ID_ROT, ID_MIRROR, ID_FILL, ID_AUDIO, ID_AUDIO_DEV, ID_STATUS, ID_VCAM_STATE, ID_INSTALL, ID_AUTOSTART,
	ID_PREVIEW, ID_TOTRAY,
	ID_TRAY_OPEN = 300, ID_TRAY_CONNECT, ID_TRAY_EXIT,
};

/* ------------------------------------------------------------------------- */
/* State                                                                     */

struct settings {
	int conn; /* 0 Wi-Fi, 1 USB */
	char host[128];
	int port;
	char camera[64];
	int width, height, fps, bitrate_kbps;
	int rotation; /* -1 auto */
	bool mirror, fill;
	bool audio;
	wchar_t audio_dev[256];
	bool autoconnect;
	bool to_tray; /* close / minimize hides the window to the notification area */
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
};

static struct {
	HINSTANCE inst;
	HWND wnd, preview, status, vcam_label, install_btn;
	HWND host, connect, camera, res, fps, bitrate, zoom, ev, zoom_label, ev_label, rot, mirror, fill, audio, audio_dev;
	HWND wifi, usb, autoconnect, autostart, totray, torch;
	NOTIFYICONDATAW nid;
	UINT taskbar_created;
	bool tray_hint_shown;
	HFONT font, font_bold;
	int dpi;

	CRITICAL_SECTION lock;
	struct settings cfg;       /* guarded by lock */
	struct phone_status ph;    /* guarded by lock */
	wchar_t net_status[256];   /* guarded by lock */
	bool connected;            /* guarded by lock */
	uint32_t *preview_bgra;    /* guarded by lock */
	bool preview_valid;        /* guarded by lock */

	volatile bool want_connect;
	volatile bool quit;
	volatile LONG gen;
	HANDLE wake, net_thread;
	SOCKET sock;               /* guarded by lock */

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

/* ------------------------------------------------------------------------- */
/* Settings                                                                  */

static void ini_get(const wchar_t *key, const wchar_t *def, wchar_t *out, int n)
{
	GetPrivateProfileStringW(L"MagerCam", key, def, out, n, app.ini);
}

static int ini_int(const wchar_t *key, int def)
{
	return (int)GetPrivateProfileIntW(L"MagerCam", key, def, app.ini);
}

static void ini_set(const wchar_t *key, const wchar_t *val)
{
	WritePrivateProfileStringW(L"MagerCam", key, val, app.ini);
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
			/* UTF-16 file so non-ASCII values survive */
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
}

static const wchar_t RUN_KEY[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

static bool autostart_enabled(void)
{
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
		return false;
	LSTATUS r = RegQueryValueExW(k, L"MagerCam", NULL, NULL, NULL, NULL);
	RegCloseKey(k);
	return r == ERROR_SUCCESS;
}

static void set_autostart(bool on)
{
	HKEY k;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_WRITE, &k) != ERROR_SUCCESS)
		return;
	if (on) {
		wchar_t exe[MAX_PATH], cmd[MAX_PATH + 32];
		GetModuleFileNameW(NULL, exe, MAX_PATH);
		_snwprintf(cmd, MAX_PATH + 32, L"\"%ls\" --minimized", exe);
		RegSetValueExW(k, L"MagerCam", 0, REG_SZ, (const BYTE *)cmd, (DWORD)(wcslen(cmd) + 1) * 2);
	} else {
		RegDeleteValueW(k, L"MagerCam");
	}
	RegCloseKey(k);
}

/* ------------------------------------------------------------------------- */
/* Network session                                                           */

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
	uint8_t *small; /* preview NV12 */
	ULONGLONG last_preview;
	int frames;
	ULONGLONG fps_t0;
	int fps_count;
	double fps_now;
};

static void on_frame(void *opaque, const uint8_t *y, const uint8_t *uv, int stride, int w, int h)
{
	struct session *s = opaque;
	EnterCriticalSection(&app.lock);
	int rot = app.cfg.rotation < 0 ? app.ph.orient : app.cfg.rotation;
	bool mirror = app.cfg.mirror, fill = app.cfg.fill;
	LeaveCriticalSection(&app.lock);

	struct nv12 src = {(uint8_t *)y, (uint8_t *)uv, stride, stride, w, h};
	vcam_write(&src, rot, mirror, fill);

	s->frames++;
	s->fps_count++;
	ULONGLONG now = GetTickCount64();
	if (now - s->fps_t0 >= 1000) {
		s->fps_now = s->fps_count * 1000.0 / (double)(now - s->fps_t0);
		s->fps_count = 0;
		s->fps_t0 = now;
	}
	/* preview: ~25 fps is plenty and keeps CPU low */
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

static void stream_session(const struct settings *cfg, LONG gen)
{
	char host[128];
	snprintf(host, sizeof(host), "%s", cfg->host);
	if (cfg->conn == 1) {
		char err[256];
		set_net_status(L"USB: подготовка adb…");
		if (!adb_prepare(NULL, cfg->port, err, sizeof(err))) {
			set_net_status_utf8(err);
			return;
		}
		snprintf(host, sizeof(host), "127.0.0.1");
	} else if (!*host) {
		set_net_status(L"Укажите IP телефона или нажмите «Найти»");
		return;
	}

	wchar_t *whost = utf8_to_wide(host);
	set_net_status(L"Подключение к %ls:%d…", whost, cfg->port);
	SOCKET sock = tcp_connect(host, cfg->port, 3000);
	if (sock == INVALID_SOCKET) {
		set_net_status(L"Нет связи с %ls:%d. Запущено ли MagerCam на телефоне?", whost, cfg->port);
		free(whost);
		return;
	}
	EnterCriticalSection(&app.lock);
	app.sock = sock;
	LeaveCriticalSection(&app.lock);

	set_recv_timeout(sock, 5000);
	char cam[200], req[1024];
	url_encode(cam, sizeof(cam), cfg->camera);
	int n = snprintf(req, sizeof(req),
			 "GET /stream?w=%d&h=%d&fps=%d&bitrate=%d&audio=%d&camera=%s HTTP/1.1\r\nHost: %s\r\n"
			 "User-Agent: MagerCam-Windows/" APP_VERSION "\r\n\r\n",
			 cfg->width, cfg->height, cfg->fps, cfg->bitrate_kbps, cfg->audio ? 1 : 0, cam, host);
	bool ok = send_all(sock, req, (size_t)n);
	int code = ok ? read_http_header(sock) : -1;
	if (code == 409) {
		set_net_status(L"Телефон занят другим клиентом");
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
		int type = hdr[2];
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
			/* new encoder session (resolution / camera change): fresh decoder */
			decoder_destroy(s.dec);
			s.dec = decoder_create(on_frame, &s);
			if (!s.dec) {
				set_net_status(L"Ошибка: декодер H.264 Windows недоступен");
				ok = false;
				break;
			}
			decoder_feed(s.dec, payload, len, pts);
			break;
		case PKT_VIDEO_FRAME:
			if (s.dec && !decoder_feed(s.dec, payload, len, pts)) {
				decoder_destroy(s.dec);
				s.dec = NULL; /* wait for the next config */
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
				if (ph.error[0])
					set_net_status(L"%ls: %ls", ph.name, ph.error);
				else
					set_net_status(L"%ls — %dx%d, %.0f к/с%ls", ph.name, ph.w, ph.h, s.fps_now, bat);
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
		stream_session(&cfg, gen);
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

/* /api/set requests run on short-lived threads so the UI never blocks */
struct control_req {
	char host[128];
	int port;
	char path[512];
};

static DWORD WINAPI control_main(void *p)
{
	struct control_req *r = p;
	http_get(r->host, r->port, r->path);
	free(r);
	return 0;
}

static void send_control(const char *fmt, ...)
{
	struct control_req *r = calloc(1, sizeof(*r));
	EnterCriticalSection(&app.lock);
	bool connected = app.connected;
	snprintf(r->host, sizeof(r->host), "%s", app.cfg.conn == 1 ? "127.0.0.1" : app.cfg.host);
	r->port = app.cfg.port;
	LeaveCriticalSection(&app.lock);
	if (!connected || !r->host[0]) {
		free(r);
		return;
	}
	char q[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(q, sizeof(q), fmt, ap);
	va_end(ap);
	snprintf(r->path, sizeof(r->path), "/api/set?%s", q);
	HANDLE h = CreateThread(NULL, 0, control_main, r, 0, NULL);
	if (h)
		CloseHandle(h);
	else
		free(r);
}

/* ------------------------------------------------------------------------- */
/* Discovery and installer (worker threads)                                  */

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

/* ------------------------------------------------------------------------- */
/* UI                                                                        */

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
		t = app.vcam_in_use ? L"Виртуальная камера «MagerCam»: используется приложением ●"
				    : L"Виртуальная камера «MagerCam»: готова. Выберите её в Zoom, Discord, Teams, браузере…";
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

/* Refreshes the controls that mirror the phone state. */
static void sync_phone_controls(void)
{
	EnterCriticalSection(&app.lock);
	struct phone_status ph = app.ph;
	char cur_cam[64];
	snprintf(cur_cam, sizeof(cur_cam), "%s", app.cfg.camera);
	LeaveCriticalSection(&app.lock);
	app.updating_ui = true;

	/* camera list */
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
	app.updating_ui = false;
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

	HWND hint = label(L"Микрофон для Zoom/Discord: установите VB-Audio Virtual Cable, выберите здесь «CABLE Input», "
			  L"а в приложении — микрофон «CABLE Output».",
			  12, 524, PREVIEW_W, 40);
	(void)hint;

	/* values */
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
	/* "Имя — 192.168.1.5" from the discovery list: keep the address */
	wchar_t *dash = wcsrchr(w, L' ');
	const wchar_t *ip = dash ? dash + 1 : w;
	EnterCriticalSection(&app.lock);
	wide_to_utf8(ip, app.cfg.host, sizeof(app.cfg.host));
	LeaveCriticalSection(&app.lock);
}

/* ---- notification area icon ---- */

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
	_snwprintf(app.nid.szTip, 127, L"MagerCam: %ls", status);
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
			reconnect(); /* the phone only sends audio when asked for it at connect time */
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
	case WM_HSCROLL:
		if ((HWND)lp == app.zoom)
			app.zoom_dirty = true;
		else if ((HWND)lp == app.ev)
			app.ev_dirty = true;
		SetTimer(w, TIMER_CONTROLS, 120, NULL);
		return 0;
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
			/* let the phone confirm before the status may move the sliders again */
			SetTimer(w, TIMER_CONTROLS + 10, 1500, NULL);
		} else if (wp == TIMER_CONTROLS + 10) {
			KillTimer(w, wp);
			app.zoom_dirty = app.ev_dirty = false;
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
	case WM_APP_INSTALLED:
		EnableWindow(app.install_btn, TRUE);
		app.vcam = vcam_start(&app.vcam_hr);
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
		tray_add(); /* Explorer restarted */
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show)
{
	(void)prev;
	/* one instance: the virtual camera and the phone connection are exclusive */
	CreateMutexW(NULL, TRUE, L"Local\\MagerCamClient");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		HWND other = FindWindowW(L"MagerCamClient", NULL);
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

	app.vcam = vcam_start(&app.vcam_hr);

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = L"MagerCamClient";
	wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
	wc.hIconSm = wc.hIcon;
	RegisterClassExW(&wc);
	RECT r = {0, 0, S(980), S(572)};
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
	AdjustWindowRect(&r, style, FALSE);
	HWND w = CreateWindowExW(0, wc.lpszClassName, L"MagerCam — телефон как веб-камера", style, CW_USEDEFAULT,
				 CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst, NULL);
	/* --minimized (autostart): only the tray icon, or a minimized window without tray */
	/* the first ShowWindow call takes the launcher's show mode, so for the tray case just never show it */
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
