/*
 * Плагин MagerCam для OBS: телефон как камера.
 * Принимаю H.264 + PCM от приложения MagerCam (см. docs/PROTOCOL.md) по Wi-Fi или по USB через adb forward.
 */
#include "compat.h"
#ifndef _WIN32
#include <cairo.h>
#include <signal.h>
#include <sys/wait.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/bmem.h>
/* беру объявление из util/threading.h: сам заголовок требует интринсиков MSVC */
EXPORT void os_set_thread_name(const char *name);

#include "ffdec.h"
#include "imgwin.h"

OBS_DECLARE_MODULE()

#define PLUGIN_VERSION "1.3.0"
/* приложение на телефоне выходит вместе с плагином: до этой версии и советую обновить старый телефон */
#define MC_PHONE_VERSION PLUGIN_VERSION
#define LOG(level, fmt, ...) blog(level, "[MagerCam] " fmt, ##__VA_ARGS__)

#define DEFAULT_PORT 4848
#define DISCOVERY_PORT 4849
/* запас в конце пакета, который FFmpeg читает за его границей; AV_INPUT_BUFFER_PADDING_SIZE во всех версиях 64 */
#define MC_INPUT_PADDING 64

enum pkt_type {
	PKT_VIDEO_CONFIG = 1,
	PKT_VIDEO_FRAME = 2,
	PKT_AUDIO_CONFIG = 3,
	PKT_AUDIO = 4,
	PKT_STATUS = 5,
	PKT_PING = 6,
};

struct found_device {
	char name[96];
	char ip[64];
	int port;
	bool auth; /* на телефоне включён код доступа */
};

#define MAX_FOUND 32
static struct found_device g_found[MAX_FOUND];
static int g_found_count;
static CRITICAL_SECTION g_found_lock;

struct mc_config {
	char host[128];
	int port;
	bool usb;
	char adb_path[512];
	int width, height, fps, bitrate_kbps;
	bool bitrate_auto; /* bitrate=auto: телефон сам подбирает битрейт под разрешение, к/с и кодек */
	bool quality;      /* enc=quality: VBR и B-кадры, задержка на кадр-два больше */
	bool audio;
	char camera[64]; /* "back", "front" или id объектива из статуса телефона */
	bool hw_decode;
	bool control;
	double zoom, ev;
	char wb[32], focus[32], antibanding[16];
	int wb_temp, wb_tint; /* для wb=manual: температура света в кельвинах и оттенок -50..50 (плюс — в пурпур) */
	bool stab;
	char key[16];     /* код доступа с экрана телефона */
	bool hevc;        /* просить H.265: вдвое меньше трафика при том же качестве */
	char profile[16]; /* "" — свои настройки, иначе call/stream/doc */
	char bg[8];       /* off/blur/image */
	double bg_strength;
	bool framing, lock;
	char sharp[12], nr[12]; /* резкость и шумоподавление камеры: off/low/default/high */
	int sharp_amount;       /* своя резкость телефона поверх камеры: -100 (мягче)..100 */
};

struct mc_source {
	obs_source_t *source;

	CRITICAL_SECTION lock;
	struct mc_config cfg;
	int rotation; /* -1 — авто */
	bool mirror;
	bool low_latency;
	bool deactivate_hidden;
	bool clear_on_disconnect;
	bool initialized;

	volatile LONG gen;
	volatile bool stop;
	volatile bool showing;
	mc_thread_t thread;
	bool has_thread;
	mc_event_t wake;
	SOCKET sock;

	/* статус от телефона, трогаю только под lock */
	char status[400];
	char phone_name[96];
	int phone_orient;
	int battery;
	bool connected;
	obs_data_array_t *cameras;
	char profile_live[16]; /* профиль, который сейчас стоит на телефоне: его и прошу при переподключении */

	/* это трогает только сетевой поток */
	struct mc_dec *dec;
	bool need_key;
	bool hw_failed;
	int64_t ts_offset;
	bool ts_valid;
	int a_rate, a_ch;
	uint8_t *rot_buf;
	size_t rot_size;
	uint8_t *payload;
	size_t payload_cap;
	bool had_video;
	bool ever_video; /* был ли хоть один кадр; до первого показываю карточку со статусом */
	bool stream_hevc; /* по флагу в VIDEO_CONFIG */
	bool stream_reorder; /* тоже по флагу: в потоке B-кадры, декодер открываю без LOW_DELAY */
	/* pts последнего показанного кадра: OBS нужны метки по возрастанию, кадр из прошлого не показываю */
	int64_t last_out_pts;
	bool out_valid;
	bool out_warned;
	int64_t st_min, st_max;
	int st_frames, st_dropped;
	/* наименьшая разница «время прихода − pts», то есть задержка сети без очереди; от неё меряю отставание */
	int64_t lat_base;
	bool lat_valid;
	bool lagging;
	bool phone_rec;   /* под lock */
	/* что умеют камера и кодер телефона, под lock: из этого строю списки в свойствах */
	int phone_sizes[32][2], phone_nsizes;
	int phone_fps[8], phone_nfps;
	/* тоже под lock: что телефон сейчас делает с картинкой (для окна настроек) и подсказка про старое приложение */
	struct mc_phone_img ph;
	char hint[320];

	/* B-кадры декодер отдаёт пачками, а OBS без буфера показывает только последний кадр пачки: развожу их по времени */
	struct mc_pacer *pacer;
	bool pace_on; /* трогает только сетевой поток */
#ifdef MC_FRAME_PROBE
	struct mc_probe *probe;
#endif
};

#ifdef _WIN32
static wchar_t *utf8_to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = bmalloc(sizeof(wchar_t) * (n + 1));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	return w;
}

static void wide_to_utf8(const wchar_t *w, char *out, size_t sz)
{
	WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)sz, NULL, NULL);
}

static bool file_exists_utf8(const char *path)
{
	wchar_t *w = utf8_to_wide(path);
	DWORD a = GetFileAttributesW(w);
	bfree(w);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

/* Статус в свойствах никто не видит, пока не нажмёт «Обновить», поэтому до первого кадра пишу его прямо
 * в картинку источника. После первого кадра молчу: на эфире короткий обрыв не должен закрывать видео. */
static void show_card(struct mc_source *s, const char *msg)
{
	const int W = 1280, H = 720;
	BITMAPINFO bi = {0};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = W;
	bi.bmiHeader.biHeight = -H;
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	void *bits = NULL;
	HDC dc = CreateCompatibleDC(NULL);
	HBITMAP bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
	if (!bmp || !bits) {
		if (dc)
			DeleteDC(dc);
		return;
	}
	HGDIOBJ old = SelectObject(dc, bmp);
	RECT r = {0, 0, W, H};
	HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
	FillRect(dc, &r, bg);
	DeleteObject(bg);
	SetBkMode(dc, TRANSPARENT);
	HFONT big = CreateFontW(96, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
	HFONT small = CreateFontW(40, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, ANTIALIASED_QUALITY, 0, L"Segoe UI");
	SelectObject(dc, big);
	SetTextColor(dc, RGB(255, 152, 0));
	RECT t1 = {0, 180, W, 320};
	DrawTextW(dc, L"MagerCam", -1, &t1, DT_CENTER | DT_SINGLELINE | DT_VCENTER);
	SelectObject(dc, small);
	SetTextColor(dc, RGB(220, 224, 232));
	wchar_t *wmsg = NULL;
	os_utf8_to_wcs_ptr(msg, 0, &wmsg);
	RECT t2 = {120, 350, W - 120, H - 80};
	if (wmsg)
		DrawTextW(dc, wmsg, -1, &t2, DT_CENTER | DT_WORDBREAK);
	bfree(wmsg);
	GdiFlush();

	struct obs_source_frame f = {0};
	f.format = VIDEO_FORMAT_BGRX;
	f.width = W;
	f.height = H;
	f.data[0] = bits;
	f.linesize[0] = W * 4;
	f.timestamp = os_gettime_ns();
	obs_source_output_video(s->source, &f);

	SelectObject(dc, old);
	DeleteObject(big);
	DeleteObject(small);
	DeleteObject(bmp);
	DeleteDC(dc);
}
#else
static bool file_exists_utf8(const char *path)
{
	return access(path, X_OK) == 0;
}

/* То же, что версия для Windows, только рисую через cairo: он есть в любом Linux с графической средой */
static void show_card(struct mc_source *s, const char *msg)
{
	const int W = 1280, H = 720;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_RGB24, W, H);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return;
	}
	cairo_t *cr = cairo_create(surf);
	cairo_set_source_rgb(cr, 24 / 255.0, 26 / 255.0, 32 / 255.0);
	cairo_paint(cr);

	cairo_text_extents_t ext;
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_BOLD);
	cairo_set_font_size(cr, 96);
	cairo_set_source_rgb(cr, 1.0, 152 / 255.0, 0);
	cairo_text_extents(cr, "MagerCam", &ext);
	cairo_move_to(cr, (W - ext.width) / 2 - ext.x_bearing, 250 - ext.height / 2 - ext.y_bearing);
	cairo_show_text(cr, "MagerCam");

	/* cairo сам строки не переносит: собираю их по словам, пока ширина влезает */
	cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, 40);
	cairo_set_source_rgb(cr, 220 / 255.0, 224 / 255.0, 232 / 255.0);
	const double max_w = W - 240;
	double y = 400;
	char line[512] = "";
	const char *p = msg;
	while (*p && y < H - 60) {
		const char *sp = strchr(p, ' ');
		size_t wl = sp ? (size_t)(sp - p) : strlen(p);
		char trial[512];
		snprintf(trial, sizeof(trial), "%s%s%.*s", line, *line ? " " : "", (int)wl, p);
		cairo_text_extents(cr, trial, &ext);
		if (ext.width > max_w && *line) {
			cairo_text_extents(cr, line, &ext);
			cairo_move_to(cr, (W - ext.width) / 2 - ext.x_bearing, y);
			cairo_show_text(cr, line);
			y += 52;
			line[0] = 0;
			continue;
		}
		snprintf(line, sizeof(line), "%s", trial);
		p += wl;
		while (*p == ' ')
			p++;
	}
	if (*line && y < H - 60) {
		cairo_text_extents(cr, line, &ext);
		cairo_move_to(cr, (W - ext.width) / 2 - ext.x_bearing, y);
		cairo_show_text(cr, line);
	}
	cairo_destroy(cr);
	cairo_surface_flush(surf);

	struct obs_source_frame f = {0};
	f.format = VIDEO_FORMAT_BGRX;
	f.width = W;
	f.height = H;
	f.data[0] = cairo_image_surface_get_data(surf);
	f.linesize[0] = (uint32_t)cairo_image_surface_get_stride(surf);
	f.timestamp = os_gettime_ns();
	obs_source_output_video(s->source, &f);
	cairo_surface_destroy(surf);
}
#endif

static void set_status(struct mc_source *s, const char *fmt, ...)
{
	char buf[400];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	EnterCriticalSection(&s->lock);
	bool changed = strcmp(buf, s->status) != 0;
	if (changed) {
		strncpy(s->status, buf, sizeof(s->status) - 1);
		LOG(LOG_INFO, "%s", buf);
	}
	bool card = changed && !s->ever_video;
	LeaveCriticalSection(&s->lock);
	if (card)
		show_card(s, buf);
}

static void url_encode(struct dstr *d, const char *v)
{
	static const char hex[] = "0123456789ABCDEF";
	for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
		    *p == '_' || *p == '.') {
			dstr_ncat(d, (const char *)p, 1);
		} else {
			char e[4] = {'%', hex[*p >> 4], hex[*p & 15], 0};
			dstr_cat(d, e);
		}
	}
}

static SOCKET tcp_connect(const char *host, int port, int timeout_ms)
{
	struct addrinfo hints = {0}, *res = NULL;
	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res)
		return INVALID_SOCKET;

	SOCKET sock = socket(res->ai_family, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		freeaddrinfo(res);
		return INVALID_SOCKET;
	}

	mc_set_nonblock(sock, true);
	int r = connect(sock, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);
	if (r == SOCKET_ERROR && !mc_connect_pending()) {
		closesocket(sock);
		return INVALID_SOCKET;
	}

	fd_set wfds, efds;
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	FD_SET(sock, &wfds);
	FD_SET(sock, &efds);
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	r = select((int)sock + 1, NULL, &wfds, &efds, &tv);
	if (r <= 0 || FD_ISSET(sock, &efds)) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
#ifndef _WIN32
	/* в Linux отказ в соединении приходит как «готов к записи», настоящий итог лежит в SO_ERROR */
	int soerr = 0;
	socklen_t elen = sizeof(soerr);
	if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &elen) != 0 || soerr != 0) {
		closesocket(sock);
		return INVALID_SOCKET;
	}
#endif

	mc_set_nonblock(sock, false);
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
	return sock;
}

static void set_recv_timeout(SOCKET sock, int ms)
{
#ifdef _WIN32
	DWORD t = (DWORD)ms;
#else
	struct timeval t = {ms / 1000, (ms % 1000) * 1000};
#endif
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
}

#ifdef MSG_NOSIGNAL
#define MC_SEND_FLAGS MSG_NOSIGNAL /* иначе оборванное соединение убьёт весь OBS сигналом SIGPIPE */
#else
#define MC_SEND_FLAGS 0
#endif

static bool recv_all(SOCKET sock, uint8_t *buf, size_t n)
{
	while (n > 0) {
		int r = (int)recv(sock, (char *)buf, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
		if (r <= 0)
			return false;
		buf += r;
		n -= (size_t)r;
	}
	return true;
}

static bool send_all(SOCKET sock, const char *buf, size_t n)
{
	while (n > 0) {
		int r = (int)send(sock, buf, (int)n, MC_SEND_FLAGS);
		if (r <= 0)
			return false;
		buf += r;
		n -= (size_t)r;
	}
	return true;
}

static int read_http_header(SOCKET sock)
{
	char hdr[4096];
	size_t len = 0;
	while (len < sizeof(hdr) - 1) {
		if (recv(sock, hdr + len, 1, 0) != 1)
			return -1;
		len++;
		if (len >= 4 && memcmp(hdr + len - 4, "\r\n\r\n", 4) == 0)
			break;
	}
	hdr[len] = 0;
	int code = -1;
	if (sscanf(hdr, "HTTP/%*s %d", &code) != 1)
		return -1;
	return code;
}

static bool http_get(const char *host, int port, const char *path, const char *key)
{
	SOCKET sock = tcp_connect(host, port, 2000);
	if (sock == INVALID_SOCKET)
		return false;
	set_recv_timeout(sock, 2000);
	struct dstr req = {0};
	dstr_printf(&req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n", path, host);
	if (key && *key)
		dstr_catf(&req, "X-MagerCam-Key: %s\r\n", key);
	dstr_cat(&req, "\r\n");
	bool ok = send_all(sock, req.array, req.len);
	dstr_free(&req);
	int code = ok ? read_http_header(sock) : -1;
	closesocket(sock);
	return code == 200;
}

#ifdef _WIN32
static bool run_process(const char *exe, const char *args, char *out, size_t outsz, DWORD timeout_ms)
{
	struct dstr cmd = {0};
	dstr_printf(&cmd, "\"%s\" %s", exe, args);
	wchar_t *wcmd = utf8_to_wide(cmd.array);
	dstr_free(&cmd);

	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		bfree(wcmd);
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr;
	si.hStdError = wr;
	si.hStdInput = NULL;
	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	bfree(wcmd);
	CloseHandle(wr);
	if (!ok) {
		CloseHandle(rd);
		return false;
	}

	size_t len = 0;
	DWORD start = GetTickCount();
	for (;;) {
		DWORD avail = 0;
		if (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0) {
			char tmp[512];
			DWORD got = 0;
			if (!ReadFile(rd, tmp, sizeof(tmp), &got, NULL) || got == 0)
				break;
			if (out && len + 1 < outsz) {
				size_t c = got < outsz - 1 - len ? got : outsz - 1 - len;
				memcpy(out + len, tmp, c);
				len += c;
			}
			continue;
		}
		if (WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0) {
			DWORD got = 0;
			char tmp[512];
			while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail > 0 &&
			       ReadFile(rd, tmp, sizeof(tmp), &got, NULL) && got > 0) {
				if (out && len + 1 < outsz) {
					size_t c = got < outsz - 1 - len ? got : outsz - 1 - len;
					memcpy(out + len, tmp, c);
					len += c;
				}
			}
			break;
		}
		if (GetTickCount() - start > timeout_ms) {
			TerminateProcess(pi.hProcess, 1);
			break;
		}
	}
	if (out)
		out[len] = 0;
	DWORD code = 1;
	GetExitCodeProcess(pi.hProcess, &code);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(rd);
	return code == 0;
}

static bool find_adb(const char *configured, char *out, size_t sz)
{
	if (configured && *configured && file_exists_utf8(configured)) {
		snprintf(out, sz, "%s", configured);
		return true;
	}

	wchar_t wpath[MAX_PATH];
	if (SearchPathW(NULL, L"adb.exe", NULL, MAX_PATH, wpath, NULL)) {
		wide_to_utf8(wpath, out, sz);
		return true;
	}

	char *bundled = obs_find_module_file(obs_current_module(), "adb/adb.exe");
	if (bundled) {
		snprintf(out, sz, "%s", bundled);
		bfree(bundled);
		if (file_exists_utf8(out))
			return true;
	}

	/* сюда adb кладёт установщик MagerCam для Windows */
	wchar_t pf[MAX_PATH];
	if (GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH)) {
		wchar_t full[MAX_PATH * 2];
		_snwprintf(full, MAX_PATH * 2, L"%ls\\MagerCam\\adb\\adb.exe", pf);
		if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
			wide_to_utf8(full, out, sz);
			return true;
		}
	}

	wchar_t local[MAX_PATH];
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
		static const wchar_t *subdirs[] = {L"\\Android\\Sdk\\platform-tools\\adb.exe",
						   L"\\Microsoft\\WinGet\\Links\\adb.exe"};
		for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
			wchar_t full[MAX_PATH * 2];
			_snwprintf(full, MAX_PATH * 2, L"%ls%ls", local, subdirs[i]);
			if (GetFileAttributesW(full) != INVALID_FILE_ATTRIBUTES) {
				wide_to_utf8(full, out, sz);
				return true;
			}
		}
	}
	return false;
}
#else
/* Аргументы adb у меня без пробелов внутри, поэтому строку просто режу по пробелам */
static bool run_process(const char *exe, const char *args, char *out, size_t outsz, DWORD timeout_ms)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", args);
	char *argv[24];
	int argc = 0;
	argv[argc++] = (char *)exe;
	char *ctx = NULL;
	for (char *t = strtok_r(buf, " ", &ctx); t && argc < 23; t = strtok_r(NULL, " ", &ctx))
		argv[argc++] = t;
	argv[argc] = NULL;

	int fds[2];
	if (pipe(fds) != 0)
		return false;
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return false;
	}
	if (pid == 0) {
		dup2(fds[1], STDOUT_FILENO);
		dup2(fds[1], STDERR_FILENO);
		close(fds[0]);
		close(fds[1]);
		execv(exe, argv);
		_exit(127);
	}
	close(fds[1]);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

	size_t len = 0;
	ULONGLONG start = GetTickCount64();
	int status = 0;
	bool exited = false;
	for (;;) {
		char tmp[512];
		ssize_t got = read(fds[0], tmp, sizeof(tmp));
		if (got > 0) {
			if (out && len + 1 < outsz) {
				size_t c = (size_t)got < outsz - 1 - len ? (size_t)got : outsz - 1 - len;
				memcpy(out + len, tmp, c);
				len += c;
			}
			continue;
		}
		if (got == 0 && exited)
			break;
		if (!exited && waitpid(pid, &status, WNOHANG) == pid) {
			exited = true;
			continue; /* дочитываю то, что осталось в трубе */
		}
		if (exited)
			break;
		if (GetTickCount64() - start > timeout_ms) {
			kill(pid, SIGKILL);
			waitpid(pid, &status, 0);
			status = 1 << 8;
			break;
		}
		Sleep(20);
	}
	close(fds[0]);
	if (out)
		out[len] = 0;
	return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool find_adb(const char *configured, char *out, size_t sz)
{
	if (configured && *configured && file_exists_utf8(configured)) {
		snprintf(out, sz, "%s", configured);
		return true;
	}
	const char *path = getenv("PATH");
	if (path) {
		char copy[4096];
		snprintf(copy, sizeof(copy), "%s", path);
		char *ctx = NULL;
		for (char *dir = strtok_r(copy, ":", &ctx); dir; dir = strtok_r(NULL, ":", &ctx)) {
			snprintf(out, sz, "%s/adb", dir);
			if (file_exists_utf8(out))
				return true;
		}
	}
	char *bundled = obs_find_module_file(obs_current_module(), "adb/adb");
	if (bundled) {
		snprintf(out, sz, "%s", bundled);
		bfree(bundled);
		if (file_exists_utf8(out))
			return true;
	}
	/* Android Studio кладёт SDK в домашнюю папку */
	const char *home = getenv("HOME");
	if (home) {
		snprintf(out, sz, "%s/Android/Sdk/platform-tools/adb", home);
		if (file_exists_utf8(out))
			return true;
	}
	return false;
}
#endif

static bool adb_prepare(struct mc_source *s, const char *adb_cfg, int port)
{
	char adb[1024];
	if (!find_adb(adb_cfg, adb, sizeof(adb))) {
		set_status(s, "USB: " MC_ADB_NAME " не найден (укажите путь в настройках)");
		return false;
	}

	char out[4096];
	run_process(adb, "start-server", NULL, 0, 15000);
	if (!run_process(adb, "devices", out, sizeof(out), 5000)) {
		set_status(s, "USB: не удалось запустить adb");
		return false;
	}

	/* беру устройства в состоянии "device": сначала USB (серийник без ':'), потом беспроводные */
	char cand[8][128];
	int ncand = 0;
	bool unauthorized = false;
	for (int pass = 0; pass < 2; pass++) {
		char copy[4096];
		snprintf(copy, sizeof(copy), "%s", out);
		char *ctx = NULL;
		for (char *line = mc_strtok(copy, "\r\n", &ctx); line; line = mc_strtok(NULL, "\r\n", &ctx)) {
			char ser[128], state[64];
			if (sscanf(line, "%127s %63s", ser, state) != 2)
				continue;
			if (strcmp(state, "unauthorized") == 0)
				unauthorized = true;
			if (strcmp(state, "device") != 0 || ncand >= 8)
				continue;
			if ((pass == 0) == (strchr(ser, ':') == NULL))
				snprintf(cand[ncand++], sizeof(cand[0]), "%s", ser);
		}
	}
	if (ncand == 0) {
		set_status(s, unauthorized ? "USB: разрешите отладку на телефоне (диалог RSA-ключа)"
					   : "USB: телефон не найден (включите отладку по USB)");
		return false;
	}

	/* если устройств несколько, выбираю то, где запущен MagerCam */
	const char *serial = cand[0];
	if (ncand > 1) {
		for (int i = 0; i < ncand; i++) {
			char args[256], pid[64];
			snprintf(args, sizeof(args), "-s %s shell pidof com.magercam.app", cand[i]);
			if (run_process(adb, args, pid, sizeof(pid), 5000) && pid[0] >= '0' && pid[0] <= '9') {
				serial = cand[i];
				break;
			}
		}
	}

	char args[256];
	snprintf(args, sizeof(args), "-s %s forward tcp:%d tcp:%d", serial, port, port);
	if (!run_process(adb, args, out, sizeof(out), 5000)) {
		set_status(s, "USB: adb forward не удался: %s", out);
		return false;
	}
	static char last_serial[128];
	if (strcmp(last_serial, serial) != 0) {
		snprintf(last_serial, sizeof(last_serial), "%s", serial);
		LOG(LOG_INFO, "USB: adb '%s', device %s", adb, serial);
	}
	/* телефон подключён кабелем: если MagerCam на нём не запущено, открываю его */
	char pid[64] = "";
	snprintf(args, sizeof(args), "-s %s shell pidof com.magercam.app", serial);
	if (!(run_process(adb, args, pid, sizeof(pid), 5000) && pid[0] >= '0' && pid[0] <= '9')) {
		set_status(s, "USB: запускаю MagerCam на телефоне…");
		snprintf(args, sizeof(args), "-s %s shell am start -n com.magercam.app/.MainActivity", serial);
		if (run_process(adb, args, NULL, 0, 8000)) {
			LOG(LOG_INFO, "USB: started MagerCam on %s", serial);
			Sleep(3500);
		}
	}
	return true;
}

static void discovery_run(int timeout_ms)
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return;
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
#ifdef _WIN32
	/* иначе ICMP «порт закрыт» от соседей по подсети рвёт приём ошибкой WSAECONNRESET */
	BOOL no_reset = FALSE;
	DWORD ret = 0;
	WSAIoctl(sock, SIO_UDP_CONNRESET, &no_reset, sizeof(no_reset), NULL, 0, &ret, NULL, NULL);
#endif
	struct sockaddr_in local = {0};
	local.sin_family = AF_INET;
	bind(sock, (struct sockaddr *)&local, sizeof(local));

	static const char msg[] = "MAGERCAM_DISCOVER";
	struct sockaddr_in dst = {0};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(DISCOVERY_PORT);
	dst.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));

	/* шлю направленный broadcast на каждый адаптер: 255.255.255.255 уходит только через одну сетевуху */
	uint32_t ips[16], masks[16];
	int nif = 0;
#ifdef _WIN32
	ULONG size = 32 * 1024;
	IP_ADAPTER_ADDRESSES *addrs = bmalloc(size);
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
				 NULL, addrs, &size) == NO_ERROR) {
		for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u && nif < 16; u = u->Next) {
				ULONG prefix = u->OnLinkPrefixLength;
				if (prefix == 0 || prefix >= 32)
					continue;
				ips[nif] = ntohl(((struct sockaddr_in *)u->Address.lpSockaddr)->sin_addr.s_addr);
				masks[nif++] = 0xFFFFFFFFu << (32 - prefix);
			}
		}
	}
	bfree(addrs);
#else
	struct ifaddrs *ifa = NULL;
	if (getifaddrs(&ifa) == 0) {
		for (struct ifaddrs *a = ifa; a && nif < 16; a = a->ifa_next) {
			if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask)
				continue;
			if (!(a->ifa_flags & IFF_UP) || (a->ifa_flags & IFF_LOOPBACK))
				continue;
			uint32_t mask = ntohl(((struct sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr);
			if (mask == 0 || mask == 0xFFFFFFFFu)
				continue;
			ips[nif] = ntohl(((struct sockaddr_in *)a->ifa_addr)->sin_addr.s_addr);
			masks[nif++] = mask;
		}
		freeifaddrs(ifa);
	}
#endif
	for (int i = 0; i < nif; i++) {
		uint32_t ip = ips[i], mask = masks[i];
		dst.sin_addr.s_addr = htonl((ip & mask) | ~mask);
		sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
		/* broadcast часто глушит роутер или сам телефон в экономии энергии, а на прямой запрос
		 * он ответит, и брандмауэр пропустит ответ. Поэтому опрашиваю ещё всю свою /24 */
		uint32_t net = ip & 0xFFFFFF00u;
		for (uint32_t h = 1; h < 255; h++) {
			if ((net | h) == ip)
				continue;
			dst.sin_addr.s_addr = htonl(net | h);
			sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
		}
	}

	struct found_device found[MAX_FOUND];
	int count = 0;
	uint64_t deadline = os_gettime_ns() + (uint64_t)timeout_ms * 1000000ULL;
	for (;;) {
		uint64_t now = os_gettime_ns();
		if (now >= deadline)
			break;
		int left = (int)((deadline - now) / 1000000ULL);
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		struct timeval tv = {left / 1000, (left % 1000) * 1000};
		if (select((int)sock + 1, &fds, NULL, NULL, &tv) <= 0)
			break;
		char buf[1024];
		struct sockaddr_in from;
		socklen_t fromlen = sizeof(from);
		int r = (int)recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
		if (r <= 0)
			continue;
		buf[r] = 0;
		obs_data_t *d = obs_data_create_from_json(buf);
		if (!d)
			continue;
		if (obs_data_get_int(d, "magercam") >= 1 && count < MAX_FOUND) {
			char ip[64];
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			bool dup = false;
			for (int i = 0; i < count; i++)
				if (strcmp(found[i].ip, ip) == 0)
					dup = true;
			if (!dup) {
				snprintf(found[count].name, sizeof(found[count].name), "%s",
					 obs_data_get_string(d, "name"));
				snprintf(found[count].ip, sizeof(found[count].ip), "%s", ip);
				found[count].port = (int)obs_data_get_int(d, "port");
				found[count].auth = obs_data_get_bool(d, "auth");
				if (!found[count].port)
					found[count].port = DEFAULT_PORT;
				count++;
			}
		}
		obs_data_release(d);
	}
	closesocket(sock);

	EnterCriticalSection(&g_found_lock);
	memcpy(g_found, found, sizeof(found[0]) * count);
	g_found_count = count;
	LeaveCriticalSection(&g_found_lock);
	LOG(LOG_INFO, "discovery: found %d device(s)", count);
}

/* FFmpeg из самого OBS, выбираю при загрузке плагина; NULL, если подходящего нет */
static const struct mc_ffapi *g_ff;

static void decoder_close(struct mc_source *s)
{
	if (s->dec && g_ff)
		g_ff->close(s->dec);
	s->dec = NULL;
}

static bool decoder_open(struct mc_source *s, bool hw)
{
	decoder_close(s);
	if (!g_ff) {
		set_status(s, "Ошибка: в этом OBS не нашлось FFmpeg (avcodec-60, 61 или 62). Нужен OBS 30 или новее");
		return false;
	}
	s->dec = g_ff->open(s->stream_hevc, s->stream_reorder, hw, &s->hw_failed);
	s->out_valid = false;
	if (!s->dec) {
		set_status(s, s->stream_hevc ? "Ошибка: не удалось открыть декодер H.265" : "Ошибка: не удалось открыть декодер H.264");
		return false;
	}
	s->need_key = true;
	return true;
}

/* Поворачиваю и отражаю одну 8-битную плоскость. src_step=2 беру для NV12, где U и V идут в одной плоскости через байт. */
static void transform_plane(const uint8_t *src, int sstride, int src_step, int w, int h, uint8_t *dst, int rot,
			    bool mirror)
{
	int dw = (rot == 90 || rot == 270) ? h : w;
	/* координаты в выходной плоскости: dx = bx + x*ax + y*ay ; dy = by + x*cx + y*cy */
	int bx, ax, ay, by, cx, cy;
	switch (rot) {
	case 90:
		bx = h - 1, ax = 0, ay = -1, by = 0, cx = 1, cy = 0;
		break;
	case 180:
		bx = w - 1, ax = -1, ay = 0, by = h - 1, cx = 0, cy = -1;
		break;
	case 270:
		bx = 0, ax = 0, ay = 1, by = w - 1, cx = -1, cy = 0;
		break;
	default:
		bx = 0, ax = 1, ay = 0, by = 0, cx = 0, cy = 1;
		break;
	}
	if (mirror) {
		bx = dw - 1 - bx;
		ax = -ax;
		ay = -ay;
	}
	const ptrdiff_t step_x = ax + (ptrdiff_t)cx * dw;
	for (int y = 0; y < h; y++) {
		const uint8_t *row = src + (ptrdiff_t)y * sstride;
		ptrdiff_t idx = (ptrdiff_t)(by + y * cy) * dw + (bx + y * ay);
		for (int x = 0; x < w; x++, idx += step_x)
			dst[idx] = row[x * src_step];
	}
}

/* pts телефона (мкс, общие часы у звука и видео) перевожу во время OBS, так звук и картинка остаются в синхроне.
 * Держу небольшой запас вперёд: иначе из-за сетевого джиттера метки звука окажутся в прошлом,
 * и OBS начнёт раздувать глобальную буферизацию звука. */
#define TS_LEAD_NS 80000000LL
static uint64_t map_timestamp(struct mc_source *s, int64_t pts_us, bool audio)
{
	int64_t now = (int64_t)os_gettime_ns();
	int64_t ts = pts_us * 1000 + s->ts_offset;
	bool late = audio ? ts < now : ts < now - 1500000000LL;
	if (!s->ts_valid || late || ts > now + 2000000000LL) {
		if (s->ts_valid && audio)
			LOG(LOG_DEBUG, "audio late by %lld ms, resync", (long long)((now - ts) / 1000000));
		s->ts_offset = now - pts_us * 1000 + TS_LEAD_NS;
		s->ts_valid = true;
		ts = now + TS_LEAD_NS;
	}
	return (uint64_t)ts;
}

/* С B-кадрами телефон отдаёт опорный кадр и следом зависевшие от него, и декодер выдаёт кадры пачкой по два-три
 * разом. В режиме без буфера OBS на каждом такте берёт только самый свежий кадр, остальные из пачки пропадают, и
 * картинка дёргается. Буфер OBS тут не годится: он раз за разом копит задержку после каждой заминки Wi-Fi и сам
 * её не отдаёт. Поэтому развожу кадры сам: показываю кадр в pts + D, где D — самое позднее «время прихода − pts»
 * за последние секунды. Опоздавший кадр сразу поднимает D, а вниз D сползает понемногу, чтобы уменьшение задержки
 * не сжимало кадры в один такт. Задержка выходит такой, какая нужна, чтобы не терять кадры. */
#define PACE_SLOTS 8

struct pace_slot {
	struct obs_source_frame2 f;
	uint8_t *buf;
	size_t cap;
	int64_t due_us;
};

struct mc_pacer {
	struct mc_source *s;
	CRITICAL_SECTION lock;
	mc_event_t wake;
	mc_thread_t thread;
	bool has_thread;
	volatile bool stop;
	struct pace_slot q[PACE_SLOTS];
	int head, count;
	bool busy; /* голова очереди сейчас уходит в OBS, её не трогаю */
	int64_t delay_us;
	bool delay_valid;
	int64_t bucket_max[2];
	int64_t bucket_start;
	bool full_warned;
#ifdef _WIN32
	HANDLE timer;
#endif
};

static inline int64_t now_us(void)
{
	return (int64_t)(os_gettime_ns() / 1000);
}

static bool copy_frame2(struct obs_source_frame2 *dst, uint8_t **buf, size_t *cap, const struct obs_source_frame2 *f)
{
	int planes = f->format == VIDEO_FORMAT_NV12 ? 2 : 3;
	size_t sz[3] = {0}, total = 0;
	for (int i = 0; i < planes; i++) {
		size_t rows = i ? (f->height + 1) / 2 : f->height;
		sz[i] = (size_t)f->linesize[i] * rows;
		total += sz[i];
	}
	if (total > *cap) {
		bfree(*buf);
		*buf = bmalloc(total);
		*cap = total;
	}
	*dst = *f;
	uint8_t *d = *buf;
	for (int i = 0; i < planes; i++) {
		memcpy(d, f->data[i], sz[i]);
		dst->data[i] = d;
		d += sz[i];
	}
	for (int i = planes; i < MAX_AV_PLANES; i++)
		dst->data[i] = NULL;
	return true;
}

#ifdef MC_FRAME_PROBE
/* Отладка плавности (сборка с -DMC_FRAME_PROBE): в угол каждого кадра пишу его номер полосками, а в готовом
 * выходе OBS читаю, какие номера реально показаны. Так видно пропуски, а не только число отправленных кадров. */
struct mc_probe {
	volatile long decoded, sent, rendered, skipped, repeated, misses;
	int last_code;
	bool have_last;
	uint64_t t0;
	uint8_t *buf;
	size_t cap;
	bool no_pacer;
	int64_t delay_sum;
	long delay_n;
	struct mc_source *s;
};

#define PROBE_BITS 16
#define PROBE_BLOCK 16

static void probe_stamp(struct obs_source_frame2 *f, int code)
{
	uint8_t *y = f->data[0];
	for (int bi = 0; bi < PROBE_BITS + 2; bi++) {
		int bit = bi == 0 ? 1 : bi == 1 ? 0 : (code >> (PROBE_BITS + 1 - bi)) & 1;
		uint8_t v = bit ? 235 : 16;
		for (int row = 0; row < PROBE_BLOCK && row < (int)f->height; row++)
			for (int x = bi * PROBE_BLOCK; x < (bi + 1) * PROBE_BLOCK && x < (int)f->width; x++)
				y[(size_t)row * f->linesize[0] + x] = v;
	}
}

static void probe_render(void *param, struct video_data *frame)
{
	struct mc_probe *pr = param;
	const uint8_t *y = frame->data[0];
	uint32_t ls = frame->linesize[0];
	if (!y)
		return;
	const uint8_t *row = y + (size_t)ls * (PROBE_BLOCK / 2);
#define PROBE_AT(bi) row[(bi) * PROBE_BLOCK + PROBE_BLOCK / 2]
	if (!(PROBE_AT(0) > 128 && PROBE_AT(1) < 128)) {
		pr->misses++;
	} else {
		int code = 0;
		for (int bi = 2; bi < PROBE_BITS + 2; bi++)
			code = (code << 1) | (PROBE_AT(bi) > 128);
		if (!pr->have_last) {
			pr->rendered++;
		} else if (code == pr->last_code) {
			pr->repeated++;
		} else {
			int d = (code - pr->last_code) & 0xFFFF;
			pr->rendered++;
			if (d < 1000)
				pr->skipped += d - 1;
		}
		pr->last_code = code;
		pr->have_last = true;
	}
#undef PROBE_AT
	uint64_t now = os_gettime_ns();
	if (!pr->t0)
		pr->t0 = now;
	if (now - pr->t0 >= 5000000000ULL) {
		LOG(LOG_INFO, "probe %s: decoded %ld, sent to OBS %ld, rendered %ld, skipped %ld, repeated %ld, unreadable %ld, "
			      "pacer %s, avg wait in pacer %.1f ms",
		    obs_source_get_name(pr->s->source), pr->decoded, pr->sent, pr->rendered, pr->skipped, pr->repeated, pr->misses, pr->no_pacer ? "off" : "on",
		    pr->delay_n ? pr->delay_sum / (double)pr->delay_n / 1000.0 : 0.0);
		pr->decoded = pr->sent = pr->rendered = pr->skipped = pr->repeated = pr->misses = 0;
		pr->delay_sum = 0;
		pr->delay_n = 0;
		pr->t0 = now;
	}
}
#endif

static void pace_output(struct mc_source *s, const struct obs_source_frame2 *f)
{
#ifdef MC_FRAME_PROBE
	if (s->probe)
		s->probe->sent++;
#endif
	obs_source_output_video2(s->source, f);
}

static void pacer_sleep(struct mc_pacer *p, int64_t us)
{
#ifdef _WIN32
	if (us >= 0 && p->timer) {
		LARGE_INTEGER due;
		due.QuadPart = -(LONGLONG)us * 10;
		SetWaitableTimer(p->timer, &due, 0, NULL, NULL, FALSE);
		HANDLE h[2] = {p->wake, p->timer};
		WaitForMultipleObjects(2, h, FALSE, 1000);
	} else {
		WaitForSingleObject(p->wake, us < 0 ? 200 : (DWORD)(us / 1000 + 1));
	}
#else
	if (us < 0) {
		mc_event_wait(p->wake, 200);
	} else if (us > 2000) {
		mc_event_wait(p->wake, (DWORD)((us - 1000) / 1000));
	} else {
		struct timespec ts = {0, (long)us * 1000};
		nanosleep(&ts, NULL);
	}
#endif
}

MC_THREAD_FN(pacer_thread, param)
{
	struct mc_pacer *p = param;
	os_set_thread_name("magercam-pace");
	while (!p->stop) {
		struct pace_slot *sl = NULL;
		int64_t wait = -1;
		EnterCriticalSection(&p->lock);
		if (p->count) {
			int64_t left = p->q[p->head].due_us - now_us();
			if (left > 0) {
				wait = left;
			} else {
				sl = &p->q[p->head];
				p->busy = true;
			}
		}
		LeaveCriticalSection(&p->lock);
		if (!sl) {
			pacer_sleep(p, wait);
			continue;
		}
		pace_output(p->s, &sl->f);
		EnterCriticalSection(&p->lock);
		p->busy = false;
		if (p->count) {
			p->head = (p->head + 1) % PACE_SLOTS;
			p->count--;
		}
		LeaveCriticalSection(&p->lock);
	}
	MC_THREAD_RETURN;
}

static struct mc_pacer *pacer_create(struct mc_source *s)
{
	struct mc_pacer *p = bzalloc(sizeof(*p));
	p->s = s;
	InitializeCriticalSection(&p->lock);
	p->wake = mc_event_create();
#ifdef _WIN32
	/* обычный таймер Windows тикает по 15,6 мс, а кадры 60 к/с идут через 16,7: нужен таймер высокого разрешения */
	p->timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
	if (!p->timer)
		p->timer = CreateWaitableTimerW(NULL, FALSE, NULL);
#endif
	p->has_thread = mc_thread_start(&p->thread, pacer_thread, p);
	return p;
}

static void pacer_destroy(struct mc_pacer *p)
{
	if (!p)
		return;
	p->stop = true;
	mc_event_set(p->wake);
	if (p->has_thread)
		mc_thread_join(p->thread);
#ifdef _WIN32
	if (p->timer)
		CloseHandle(p->timer);
#endif
	mc_event_destroy(p->wake);
	DeleteCriticalSection(&p->lock);
	for (int i = 0; i < PACE_SLOTS; i++)
		bfree(p->q[i].buf);
	bfree(p);
}

/* wait — дождаться, пока кадр, который уже уходит в OBS, уйдёт: после этого можно чистить картинку */
static void pacer_flush(struct mc_pacer *p, bool wait)
{
	EnterCriticalSection(&p->lock);
	p->count = p->busy ? 1 : 0;
	p->delay_valid = false;
	LeaveCriticalSection(&p->lock);
	while (wait) {
		EnterCriticalSection(&p->lock);
		bool busy = p->busy;
		LeaveCriticalSection(&p->lock);
		if (!busy)
			break;
		Sleep(1);
	}
}

static void pacer_push(struct mc_pacer *p, const struct obs_source_frame2 *f, int64_t pts_us)
{
	int64_t now = now_us();
	int64_t d = now - pts_us;
	EnterCriticalSection(&p->lock);
	/* больше секунды разницы — у телефона новые часы, старая оценка ни к чему */
	if (!p->delay_valid || d > p->delay_us || p->delay_us - d > 1000000) {
		if (!p->delay_valid || p->delay_us - d > 1000000) {
			p->bucket_max[0] = p->bucket_max[1] = d;
			p->bucket_start = now;
		}
		p->delay_us = d;
		p->delay_valid = true;
	}
	if (now - p->bucket_start >= 1000000) {
		p->bucket_max[1] = p->bucket_max[0];
		p->bucket_max[0] = d;
		p->bucket_start = now;
	} else if (d > p->bucket_max[0]) {
		p->bucket_max[0] = d;
	}
	int64_t recent = p->bucket_max[0] > p->bucket_max[1] ? p->bucket_max[0] : p->bucket_max[1];
	if (p->delay_us > recent + 2000) {
		/* лишнее срезаю на шестнадцатую часть за кадр, но не меньше миллисекунды: большой запас после заминки
		 * уходит за секунду-другую, а маленький сползает почти незаметно для плавности */
		int64_t excess = p->delay_us - recent - 2000;
		int64_t step = excess / 16 > 1000 ? excess / 16 : 1000;
		p->delay_us -= step < excess ? step : excess;
	}
	if (p->count >= PACE_SLOTS) {
		if (!p->full_warned)
			LOG(LOG_WARNING, "pacer queue full, dropping a frame");
		p->full_warned = true;
		LeaveCriticalSection(&p->lock);
		return;
	}
	struct pace_slot *sl = &p->q[(p->head + p->count) % PACE_SLOTS];
	copy_frame2(&sl->f, &sl->buf, &sl->cap, f);
	sl->due_us = pts_us + p->delay_us;
	p->count++;
#ifdef MC_FRAME_PROBE
	if (p->s->probe) {
		p->s->probe->delay_sum += p->delay_us - d; /* сколько кадр ждёт в очереди сверх прихода */
		p->s->probe->delay_n++;
	}
#endif
	LeaveCriticalSection(&p->lock);
	mc_event_set(p->wake);
}

static void output_frame(struct mc_source *s, const struct mc_frame *f, int rot, bool mirror, bool pace)
{
	struct obs_source_frame2 out = {0};
	enum video_format fmt;
	switch (f->pix) {
	case MC_PIX_I420:
		fmt = VIDEO_FORMAT_I420;
		break;
	case MC_PIX_NV12:
		fmt = VIDEO_FORMAT_NV12;
		break;
	default:
		LOG(LOG_WARNING, "unsupported pixel format %d", f->raw_format);
		return;
	}

	enum video_colorspace cs;
	switch (f->cs) {
	case MC_CS_709:
		cs = VIDEO_CS_709;
		break;
	case MC_CS_601:
		cs = VIDEO_CS_601;
		break;
	default:
		cs = f->height >= 720 || f->width >= 1280 ? VIDEO_CS_709 : VIDEO_CS_601;
		break;
	}
	out.range = f->full_range ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

	int w = f->width, h = f->height;
	if (rot == 0 && !mirror) {
		out.format = fmt;
		out.width = w;
		out.height = h;
		for (int i = 0; i < 3; i++) {
			out.data[i] = f->data[i];
			out.linesize[i] = f->linesize[i];
		}
	} else {
		/* повёрнутый кадр всегда отдаю в I420, плоскости плотно, stride == width */
		w &= ~1;
		h &= ~1;
		int ow = (rot == 90 || rot == 270) ? h : w;
		int oh = (rot == 90 || rot == 270) ? w : h;
		int cw = w / 2, ch = h / 2;
		size_t need = (size_t)ow * oh * 3 / 2;
		if (need > s->rot_size) {
			bfree(s->rot_buf);
			s->rot_buf = bmalloc(need);
			s->rot_size = need;
		}
		uint8_t *y = s->rot_buf;
		uint8_t *u = y + (size_t)ow * oh;
		uint8_t *v = u + (size_t)(ow / 2) * (oh / 2);
		transform_plane(f->data[0], f->linesize[0], 1, w, h, y, rot, mirror);
		if (fmt == VIDEO_FORMAT_I420) {
			transform_plane(f->data[1], f->linesize[1], 1, cw, ch, u, rot, mirror);
			transform_plane(f->data[2], f->linesize[2], 1, cw, ch, v, rot, mirror);
		} else {
			transform_plane(f->data[1], f->linesize[1], 2, cw, ch, u, rot, mirror);
			transform_plane(f->data[1] + 1, f->linesize[1], 2, cw, ch, v, rot, mirror);
		}
		out.format = VIDEO_FORMAT_I420;
		out.width = ow;
		out.height = oh;
		out.data[0] = y;
		out.data[1] = u;
		out.data[2] = v;
		out.linesize[0] = ow;
		out.linesize[1] = ow / 2;
		out.linesize[2] = ow / 2;
	}

	video_format_get_parameters_for_format(cs, out.range, out.format, out.color_matrix, out.color_range_min,
					       out.color_range_max);

	int64_t pts_us = f->pts_us;
	out.timestamp = map_timestamp(s, pts_us, false);
#ifdef MC_FRAME_PROBE
	if (s->probe && s->probe->no_pacer)
		pace = false;
#endif
	if (pace != s->pace_on) {
		/* без развода кадры идут сразу, и то, что ещё ждёт в очереди, вышло бы позже них */
		if (!pace)
			pacer_flush(s->pacer, true);
		LOG(LOG_INFO, "video: %s", pace ? "b-frames, pacing decoder output" : "showing frames as they come");
		s->pace_on = pace;
	}
#ifdef MC_FRAME_PROBE
	struct mc_probe *pr = s->probe;
	if (pr) {
		int code = (int)(pr->decoded++ & 0xFFFF);
		/* штамп пишу в копию: кадр декодера может быть опорным для следующих */
		copy_frame2(&out, &pr->buf, &pr->cap, &out);
		probe_stamp(&out, code);
	}
#endif
	if (pace)
		pacer_push(s->pacer, &out, pts_us);
	else
		pace_output(s, &out);
	s->had_video = true;
	s->ever_video = true;
}

struct frame_ctx {
	struct mc_source *s;
	int rot;
	bool mirror;
	bool pace;
};

static void on_frame(void *opaque, const struct mc_frame *f)
{
	struct frame_ctx *c = opaque;
	struct mc_source *s = c->s;
	/* Кадр старше уже показанного бывает, только если декодер отдал B-кадры в порядке декодирования.
	 * Такой кадр пропускаю: дёрнуть картинку назад хуже, чем потерять кадр. Скачок больше секунды — это
	 * новые часы телефона, а не перестановка. */
	if (s->out_valid && f->pts_us <= s->last_out_pts && s->last_out_pts - f->pts_us < 1000000) {
		if (!s->out_warned)
			LOG(LOG_WARNING, "decoder returned frame %lld us older than the previous one, dropping it",
			    (long long)(s->last_out_pts - f->pts_us));
		s->out_warned = true;
		s->st_dropped++;
		return;
	}
	/* шаг между метками соседних кадров: по нему в логе (--verbose) видно, ровно ли идёт видео */
	if (s->out_valid) {
		int64_t step = f->pts_us - s->last_out_pts;
		if (!s->st_frames || step < s->st_min)
			s->st_min = step;
		if (!s->st_frames || step > s->st_max)
			s->st_max = step;
		if (++s->st_frames == 300) {
			LOG(LOG_DEBUG, "video: 300 frames, pts step %.1f..%.1f ms, %d dropped as out of order%s",
			    s->st_min / 1000.0, s->st_max / 1000.0, s->st_dropped, s->stream_reorder ? ", b-frames" : "");
			s->st_frames = 0;
			s->st_dropped = 0;
		}
	}
	s->last_out_pts = f->pts_us;
	s->out_valid = true;
	output_frame(s, f, c->rot, c->mirror, c->pace);
}

static void decode_packet(struct mc_source *s, const uint8_t *data, size_t len, int64_t pts, bool key, int rot,
			  bool mirror, bool low_latency)
{
	if (!s->dec)
		return;
	if (s->need_key && !key)
		return;
	s->need_key = false;

	struct frame_ctx c = {s, rot, mirror, s->stream_reorder && low_latency};
	switch (g_ff->decode(s->dec, data, len, pts, key, on_frame, &c)) {
	case MC_DEC_NEED_KEY:
		s->need_key = true;
		break;
	case MC_DEC_HW_FAILED:
		LOG(LOG_WARNING, "hw transfer failed, switching to software decoding");
		s->hw_failed = true;
		decoder_open(s, false);
		break;
	default:
		break;
	}
}

/* obs_data не держит массивы строк и чисел, поэтому "sizes" и "fps_list" разбираю из текста сам */
static const char *json_array_body(const char *json, const char *key)
{
	char pat[32];
	snprintf(pat, sizeof(pat), "\"%s\"", key);
	const char *p = strstr(json, pat);
	if (!p)
		return NULL;
	p = strchr(p + strlen(pat), '[');
	return p ? p + 1 : NULL;
}

static void parse_formats(struct mc_source *s, const char *json)
{
	int sizes[32][2], ns = 0, fps[8], nf = 0;
	const char *p = json_array_body(json, "sizes");
	while (p && *p && *p != ']' && ns < 32) {
		int w, h;
		if (*p == '"' && sscanf(p + 1, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240) {
			sizes[ns][0] = w;
			sizes[ns][1] = h;
			ns++;
			p = strchr(p + 1, '"');
			if (!p)
				break;
		}
		p++;
	}
	p = json_array_body(json, "fps_list");
	while (p && *p && *p != ']' && nf < 8) {
		if (*p >= '0' && *p <= '9') {
			fps[nf++] = (int)strtol(p, (char **)&p, 10);
			continue;
		}
		p++;
	}
	EnterCriticalSection(&s->lock);
	if (ns) {
		memcpy(s->phone_sizes, sizes, sizeof(sizes));
		s->phone_nsizes = ns;
	}
	if (nf) {
		memcpy(s->phone_fps, fps, sizeof(fps));
		s->phone_nfps = nf;
	}
	LeaveCriticalSection(&s->lock);
}

/* массив строк ["off","low"] складываю в "off,low" */
static void parse_str_list(const char *json, const char *key, char *out, size_t sz)
{
	out[0] = 0;
	const char *p = json_array_body(json, key);
	size_t n = 0;
	while (p && *p && *p != ']') {
		if (*p == '"') {
			const char *e = strchr(p + 1, '"');
			if (!e)
				break;
			int len = (int)(e - p - 1);
			if (n + (size_t)len + 2 < sz)
				n += (size_t)snprintf(out + n, sz - n, "%s%.*s", n ? "," : "", len, p + 1);
			p = e;
		}
		p++;
	}
}

static void read_phone_img(struct mc_phone_img *ph, obs_data_t *d, const char *json)
{
	ph->valid = true;
	ph->seq++;
	snprintf(ph->name, sizeof(ph->name), "%s", obs_data_get_string(d, "name"));
	snprintf(ph->app, sizeof(ph->app), "%s", obs_data_get_string(d, "app"));
	ph->zoom = obs_data_get_double(d, "zoom");
	ph->zoom_min = obs_data_has_user_value(d, "zoom_min") ? obs_data_get_double(d, "zoom_min") : 1.0;
	ph->zoom_max = obs_data_get_double(d, "zoom_max");
	ph->ev = obs_data_get_double(d, "ev");
	ph->ev_min = obs_data_get_double(d, "ev_min");
	ph->ev_max = obs_data_get_double(d, "ev_max");
	ph->bg_strength = obs_data_get_double(d, "bg_strength");
	snprintf(ph->wb, sizeof(ph->wb), "%s", obs_data_get_string(d, "wb"));
	snprintf(ph->focus, sizeof(ph->focus), "%s", obs_data_get_string(d, "focus"));
	snprintf(ph->antibanding, sizeof(ph->antibanding), "%s", obs_data_get_string(d, "antibanding"));
	snprintf(ph->bg, sizeof(ph->bg), "%s", obs_data_get_string(d, "bg"));
	snprintf(ph->profile, sizeof(ph->profile), "%s", obs_data_get_string(d, "profile"));
	ph->has_wb_temp = obs_data_has_user_value(d, "wb_temp");
	ph->wb_temp = (int)obs_data_get_int(d, "wb_temp");
	ph->wb_tint = (int)obs_data_get_int(d, "wb_tint");
	ph->has_wb_manual = obs_data_get_bool(d, "wb_manual_available");
	ph->has_flash = obs_data_get_bool(d, "has_flash");
	ph->torch = obs_data_get_bool(d, "torch");
	ph->framing_available = !obs_data_has_user_value(d, "framing_available") || obs_data_get_bool(d, "framing_available");
	ph->framing = obs_data_get_bool(d, "framing");
	ph->stab = obs_data_get_bool(d, "stab");
	ph->lock = obs_data_get_bool(d, "lock");
	ph->rec = obs_data_get_bool(d, "rec");
	ph->has_sharp = obs_data_has_user_value(d, "sharp");
	ph->has_nr = obs_data_has_user_value(d, "nr");
	snprintf(ph->sharp, sizeof(ph->sharp), "%s", obs_data_get_string(d, "sharp"));
	snprintf(ph->nr, sizeof(ph->nr), "%s", obs_data_get_string(d, "nr"));
	parse_str_list(json, "sharp_modes", ph->sharp_modes, sizeof(ph->sharp_modes));
	parse_str_list(json, "nr_modes", ph->nr_modes, sizeof(ph->nr_modes));
	ph->has_sharp_amount = obs_data_has_user_value(d, "sharp_amount");
	ph->sharp_amount = (int)obs_data_get_int(d, "sharp_amount");
	ph->has_exp = obs_data_has_user_value(d, "exp_us");
	ph->exp_us = (int)obs_data_get_int(d, "exp_us");
	ph->iso = (int)obs_data_get_int(d, "iso");
	ph->iso_max = (int)obs_data_get_int(d, "iso_max");
	ph->frame_us = (int)obs_data_get_int(d, "exp_max_us");
}

/* Телефон старше плагина молча пропускает незнакомые ключи, и ручной баланс или авто-битрейт просто не работают.
 * Узнаю это по отсутствию полей в статусе и пишу в строку статуса; в лог — один раз, пока подсказка не сменится. */
static void update_old_phone_hint(struct mc_source *s, obs_data_t *d)
{
	const struct mc_config *c = &s->cfg;
	const char *miss[4];
	int n = 0;
	if (c->control && strcmp(c->wb, "manual") == 0 && !obs_data_has_user_value(d, "wb_manual_available"))
		miss[n++] = "ручной баланс белого";
	if (c->bitrate_auto && !s->profile_live[0] && !obs_data_has_user_value(d, "bitrate_auto"))
		miss[n++] = "авто-битрейт";
	if (c->quality && !obs_data_has_user_value(d, "enc"))
		miss[n++] = "режим «Качество»";
	if (c->control && (strcmp(c->sharp, "default") != 0 || strcmp(c->nr, "default") != 0) &&
	    !obs_data_has_user_value(d, "sharp"))
		miss[n++] = "резкость и шумоподавление";
	else if (c->control && c->sharp_amount && !obs_data_has_user_value(d, "sharp_amount"))
		miss[n++] = "доп. резкость";
	char hint[sizeof(s->hint)] = "";
	if (n) {
		char list[200] = "";
		for (int i = 0; i < n; i++) {
			size_t o = strlen(list);
			snprintf(list + o, sizeof(list) - o, "%s%s", i == 0 ? "" : i == n - 1 ? " и " : ", ", miss[i]);
		}
		const char *app = obs_data_get_string(d, "app");
		char ver[40] = "";
		if (*app)
			snprintf(ver, sizeof(ver), " (%s)", app);
		snprintf(hint, sizeof(hint), "На телефоне старая версия MagerCam%s — обновите до " MC_PHONE_VERSION
			 ", иначе не %s %s", ver, n > 1 ? "работают" : "работает", list);
	}
	if (strcmp(hint, s->hint) != 0) {
		snprintf(s->hint, sizeof(s->hint), "%s", hint);
		if (*hint)
			LOG(LOG_WARNING, "%s", hint);
	}
}

static void apply_status_json(struct mc_source *s, const char *json)
{
	obs_data_t *d = obs_data_create_from_json(json);
	if (!d)
		return;
	parse_formats(s, json);
	EnterCriticalSection(&s->lock);
	s->phone_orient = (int)obs_data_get_int(d, "orient");
	s->battery = (int)obs_data_get_int(d, "battery");
	snprintf(s->phone_name, sizeof(s->phone_name), "%s", obs_data_get_string(d, "name"));
	obs_data_array_t *cams = obs_data_get_array(d, "cameras");
	if (cams) {
		obs_data_array_release(s->cameras);
		s->cameras = cams;
	}
	s->phone_rec = obs_data_get_bool(d, "rec");
	snprintf(s->profile_live, sizeof(s->profile_live), "%s", obs_data_get_string(d, "profile"));
	read_phone_img(&s->ph, d, json);
	update_old_phone_hint(s, d);
	LeaveCriticalSection(&s->lock);
	char extra[256] = "";
	int br = (int)obs_data_get_int(d, "bitrate_now");
	if (br > 0)
		snprintf(extra, sizeof(extra), ", %s %.1f Мбит/с%s", strcmp(obs_data_get_string(d, "codec"), "hevc") ? "H.264" : "H.265",
			 br / 1000.0, obs_data_get_bool(d, "bitrate_auto") ? " (авто)" : "");
	if (strcmp(obs_data_get_string(d, "enc"), "quality") == 0) {
		size_t o = strlen(extra);
		snprintf(extra + o, sizeof(extra) - o, obs_data_get_bool(d, "bframes") ? ", «Качество», B-кадры" : ", «Качество»");
	}
	if (obs_data_get_bool(d, "rec")) {
		long long ms = obs_data_get_int(d, "rec_ms");
		size_t o = strlen(extra);
		snprintf(extra + o, sizeof(extra) - o, ", запись %02lld:%02lld", ms / 60000, ms / 1000 % 60);
	}
	if (obs_data_get_bool(d, "hot")) {
		size_t o = strlen(extra);
		snprintf(extra + o, sizeof(extra) - o, ", телефон перегрет");
	}
	set_status(s, "Подключено: %s — %dx%d@%d%s, батарея %d%%%s", obs_data_get_string(d, "name"),
		   (int)obs_data_get_int(d, "w"), (int)obs_data_get_int(d, "h"), (int)obs_data_get_int(d, "fps"), extra,
		   (int)obs_data_get_int(d, "battery"), obs_data_get_bool(d, "charging") ? " (заряжается)" : "");
	obs_data_release(d);
}

/* Если на телефоне стоял профиль, прошу его, а то, что он задаёт сам, не шлю: иначе телефон
 * примет мои разрешение и камеру за ручную правку и сбросит профиль при первом же переподключении. */
static void build_stream_query(struct dstr *q, const struct mc_config *c, const char *profile)
{
	dstr_printf(q, "/stream?audio=%d", c->audio ? 1 : 0);
	if (*profile) {
		dstr_cat(q, "&profile=");
		url_encode(q, profile);
	} else {
		dstr_catf(q, "&w=%d&h=%d&fps=%d", c->width, c->height, c->fps);
		/* старый телефон "auto" не разберёт как число и просто оставит свой битрейт */
		if (c->bitrate_auto)
			dstr_cat(q, "&bitrate=auto");
		else
			dstr_catf(q, "&bitrate=%d", c->bitrate_kbps);
		dstr_cat(q, "&camera=");
		url_encode(q, c->camera);
	}
	if (c->hevc && g_ff && g_ff->has_hevc())
		dstr_cat(q, "&codec=hevc");
	/* bframes=1: B-кадры я переупорядочу, флаг в VIDEO_CONFIG скажет, когда они пойдут */
	dstr_catf(q, "&enc=%s&bframes=1", c->quality ? "quality" : "latency");
	if (c->control) {
		if (!*profile) {
			dstr_catf(q, "&framing=%d&bg=%s&stab=%d", c->framing ? 1 : 0, c->bg, c->stab ? 1 : 0);
			dstr_cat(q, "&focus=");
			url_encode(q, c->focus);
		}
		dstr_catf(q, "&lock=%d&bg_strength=%.2f&zoom=%.2f&ev=%.2f", c->lock ? 1 : 0, c->bg_strength, c->zoom,
			  c->ev);
		dstr_cat(q, "&wb=");
		url_encode(q, c->wb);
		/* телефон до ручного баланса примет manual за авто, а температуру пропустит */
		if (strcmp(c->wb, "manual") == 0)
			dstr_catf(q, "&wb_temp=%d&wb_tint=%d", c->wb_temp, c->wb_tint);
		dstr_cat(q, "&antibanding=");
		url_encode(q, c->antibanding);
		/* старый телефон эти ключи пропустит */
		dstr_catf(q, "&sharp=%s&nr=%s&sharp_amount=%d", c->sharp, c->nr, c->sharp_amount);
	}
}

/* Если декодер не успевает (4K в HEVC программно) или в сети скопился хвост, кадры приходят всё позже и позже,
 * и задержка уже никогда не уходит. Тогда пропускаю кадры до ключевого: читать сокет без декодирования быстро,
 * хвост выбирается за доли секунды, и картинка снова живая. */
#define LAG_LIMIT_US 400000LL
static bool video_lagging(struct mc_source *s, int64_t pts_us, bool key)
{
	int64_t d = (int64_t)(os_gettime_ns() / 1000) - pts_us;
	/* базу понемногу поднимаю, чтобы уход часов телефона и ПК не копился; реальный минимум её тут же опустит */
	if (!s->lat_valid || d < s->lat_base) {
		s->lat_base = d;
		s->lat_valid = true;
	} else {
		s->lat_base += 20;
	}
	int64_t lag = d - s->lat_base;
	if (lag > LAG_LIMIT_US && !key) {
		if (!s->lagging)
			LOG(LOG_INFO, "video %lld ms behind, skipping to next keyframe", (long long)(lag / 1000));
		s->lagging = true;
		return true;
	}
	/* ключевой кадр показываю, даже если он опоздал, иначе при постоянном отставании картинка встанет совсем */
	if (s->lagging && lag <= LAG_LIMIT_US / 2)
		s->lagging = false;
	return s->lagging && !key;
}

static void wait_or_wake(struct mc_source *s, DWORD ms)
{
	if (!s->stop)
		mc_event_wait(s->wake, ms);
}

/* false — телефон отказал по коду доступа, повторять без смены кода бессмысленно */
static bool stream_session(struct mc_source *s, const struct mc_config *cfg, LONG gen)
{
	const char *host = cfg->host;
	if (cfg->usb) {
		if (!adb_prepare(s, cfg->adb_path, cfg->port))
			return true;
		host = "127.0.0.1";
	} else if (!*host) {
		set_status(s, "Укажите IP телефона или выберите его из списка");
		return true;
	}
	/* неполный код телефон засчитает как неверный, а после десяти таких заблокирует ПК на минуту */
	int digits = 0;
	for (const char *k = cfg->key; *k; k++)
		digits += *k >= '0' && *k <= '9';
	if (digits && digits != 6 && !cfg->usb) {
		set_status(s, "Код доступа — 6 цифр с экрана телефона");
		return false;
	}

	set_status(s, "Подключение к %s:%d…", host, cfg->port);
	SOCKET sock = tcp_connect(host, cfg->port, 3000);
	if (sock == INVALID_SOCKET) {
		set_status(s, "Нет связи с %s:%d — запущено ли приложение MagerCam?", host, cfg->port);
		return true;
	}
	EnterCriticalSection(&s->lock);
	s->sock = sock;
	LeaveCriticalSection(&s->lock);

	set_recv_timeout(sock, 5000);
	struct dstr q = {0}, req = {0};
	char profile[16];
	EnterCriticalSection(&s->lock);
	snprintf(profile, sizeof(profile), "%s", s->profile_live);
	LeaveCriticalSection(&s->lock);
	build_stream_query(&q, cfg, profile);
	dstr_printf(&req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MagerCam-OBS/" PLUGIN_VERSION "\r\n", q.array,
		    host);
	if (cfg->key[0])
		dstr_catf(&req, "X-MagerCam-Key: %s\r\n", cfg->key);
	dstr_cat(&req, "\r\n");
	bool ok = send_all(sock, req.array, req.len);
	dstr_free(&q);
	dstr_free(&req);

	int code = ok ? read_http_header(sock) : -1;
	bool retry = true;
	if (code == 409) {
		set_status(s, "Телефон занят: камера используется другим клиентом");
		ok = false;
	} else if (code == 401 || code == 429) {
		set_status(s, code == 429 ? "Слишком много неверных кодов, телефон ждёт минуту"
		           : cfg->key[0] ? "Неверный код доступа — введите код с экрана телефона"
					 : "Нужен код доступа: откройте свойства источника и впишите 6 цифр с экрана "
					   "телефона в поле «Код доступа»");
		retry = false;
		ok = false;
	} else if (code != 200) {
		set_status(s, "Неверный ответ телефона (%d)", code);
		ok = false;
	}

	if (ok) {
		s->stream_hevc = false;
		s->stream_reorder = false;
		s->out_warned = false;
		decoder_open(s, cfg->hw_decode);
		s->ts_valid = false;
		s->lat_valid = false;
		s->lagging = false;
		s->a_rate = 48000;
		s->a_ch = 1;
		EnterCriticalSection(&s->lock);
		s->connected = true;
		LeaveCriticalSection(&s->lock);
		set_status(s, "Подключено к %s", host);
	}

	/* телефон может жить и слать PING, но кодер у него встал (так бывало после 4K). Без кадров дольше 8 с
	 * переподключаюсь: новое подключение заставляет телефон перезапустить поток. */
	ULONGLONG last_video = GetTickCount64();
	while (ok && !s->stop && s->gen == gen) {
		if (GetTickCount64() - last_video > 8000) {
			LOG(LOG_WARNING, "no video for 8 s, reconnecting");
			break;
		}
		uint8_t hdr[16];
		if (!recv_all(sock, hdr, sizeof(hdr)))
			break;
		if (hdr[0] != 'M' || hdr[1] != 'C') {
			LOG(LOG_WARNING, "bad packet magic");
			break;
		}
		int type = hdr[2];
		bool key = hdr[3] & 1;
		uint32_t len = ((uint32_t)hdr[4] << 24) | ((uint32_t)hdr[5] << 16) | ((uint32_t)hdr[6] << 8) | hdr[7];
		int64_t pts = 0;
		for (int i = 0; i < 8; i++)
			pts = (pts << 8) | hdr[8 + i];
		if (len > 32 * 1024 * 1024) {
			LOG(LOG_WARNING, "packet too large: %u", len);
			break;
		}
		size_t need = (size_t)len + MC_INPUT_PADDING + 1;
		if (need > s->payload_cap) {
			bfree(s->payload);
			s->payload_cap = need * 2;
			s->payload = bmalloc(s->payload_cap);
		}
		if (len && !recv_all(sock, s->payload, len))
			break;
		memset(s->payload + len, 0, MC_INPUT_PADDING + 1);

		EnterCriticalSection(&s->lock);
		int rot = s->rotation < 0 ? s->phone_orient : s->rotation;
		bool mirror = s->mirror;
		bool low_latency = s->low_latency;
		LeaveCriticalSection(&s->lock);

		switch (type) {
		case PKT_VIDEO_CONFIG:
			/* новая сессия кодера: пересоздаю декодер, вдруг сменилось разрешение или кодек */
			s->stream_hevc = (hdr[3] & 2) != 0;
			s->stream_reorder = (hdr[3] & 4) != 0;
			s->lat_valid = false;
			s->lagging = false;
			decoder_open(s, cfg->hw_decode);
			decode_packet(s, s->payload, len, pts, true, rot, mirror, low_latency);
			s->need_key = true;
			break;
		case PKT_VIDEO_FRAME:
			last_video = GetTickCount64();
			if (video_lagging(s, pts, key)) {
				s->need_key = true;
				break;
			}
			decode_packet(s, s->payload, len, pts, key, rot, mirror, low_latency);
			break;
		case PKT_AUDIO_CONFIG:
			if (len >= 5) {
				int rate = (int)(((uint32_t)s->payload[0] << 24) | ((uint32_t)s->payload[1] << 16) |
						 ((uint32_t)s->payload[2] << 8) | s->payload[3]);
				/* мусорную частоту в ресемплер OBS не передаю, звук тогда просто молчит */
				s->a_rate = rate >= 8000 && rate <= 192000 ? rate : 0;
				s->a_ch = s->payload[4];
			}
			break;
		case PKT_AUDIO: {
			if (!cfg->audio || !s->a_rate || s->a_ch < 1 || s->a_ch > 2)
				break;
			struct obs_source_audio a = {0};
			a.data[0] = s->payload;
			a.frames = len / (2 * s->a_ch);
			a.speakers = s->a_ch == 2 ? SPEAKERS_STEREO : SPEAKERS_MONO;
			a.format = AUDIO_FORMAT_16BIT;
			a.samples_per_sec = s->a_rate;
			a.timestamp = map_timestamp(s, pts, true);
			obs_source_output_audio(s->source, &a);
			break;
		}
		case PKT_STATUS:
			s->payload[len] = 0;
			apply_status_json(s, (const char *)s->payload);
			break;
		default:
			break;
		}
	}

	EnterCriticalSection(&s->lock);
	s->sock = INVALID_SOCKET;
	bool was_connected = s->connected;
	s->connected = false;
	bool clear = s->clear_on_disconnect;
	s->ph.valid = false;
	s->ph.seq++;
	s->hint[0] = 0;
	LeaveCriticalSection(&s->lock);
	closesocket(sock);
	decoder_close(s);
	/* кадры прежней сессии в очереди уже ни к чему, а после чистки картинки не должно прийти ни одного */
	pacer_flush(s->pacer, true);
	s->pace_on = false;

	if (was_connected) {
		if (!s->stop && s->gen == gen)
			set_status(s, "Соединение потеряно, переподключение…");
		if (clear && s->had_video)
			obs_source_output_video2(s->source, NULL);
		s->had_video = false;
	}
	return retry;
}

MC_THREAD_FN(net_thread, param)
{
	struct mc_source *s = param;
	os_set_thread_name("magercam-net");

	LONG last_gen = s->gen;
	while (!s->stop) {
		/* настройки меняют по символу, пока печатают IP или код: жду паузы, а не подключаюсь к «192.1» */
		if (s->gen != last_gen) {
			LONG g;
			do {
				g = s->gen;
				Sleep(700);
			} while (!s->stop && s->gen != g);
		}
		LONG gen = s->gen;
		last_gen = gen;
		EnterCriticalSection(&s->lock);
		struct mc_config cfg = s->cfg;
		bool idle = s->deactivate_hidden && !s->showing;
		LeaveCriticalSection(&s->lock);

		if (idle) {
			set_status(s, "Неактивно (источник скрыт)");
			wait_or_wake(s, 1000);
			continue;
		}

		bool retry = stream_session(s, &cfg, gen);
		/* после отказа по коду жду, пока код не поменяют, иначе телефон заблокирует меня на минуту */
		while (!retry && !s->stop && s->gen == gen)
			wait_or_wake(s, 1000);
		if (s->stop)
			break;
		if (s->gen == gen)
			wait_or_wake(s, 1000);
	}

	MC_THREAD_RETURN;
}

static void force_reconnect(struct mc_source *s)
{
	InterlockedIncrement(&s->gen);
	EnterCriticalSection(&s->lock);
	if (s->sock != INVALID_SOCKET)
		shutdown(s->sock, SD_BOTH);
	LeaveCriticalSection(&s->lock);
	mc_event_set(s->wake);
}

/* Запросы /api/set шлю не из UI-потока, чтобы OBS не подвисал на сети. */

struct control_req {
	char host[128];
	int port;
	char path[512];
	char key[16];
};

MC_THREAD_FN(control_thread, param)
{
	struct control_req *r = param;
	if (!http_get(r->host, r->port, r->path, r->key))
		LOG(LOG_WARNING, "control request failed: %s", r->path);
	bfree(r);
	MC_THREAD_RETURN;
}

static void send_control(struct mc_source *s, const char *query)
{
	struct control_req *r = bzalloc(sizeof(*r));
	EnterCriticalSection(&s->lock);
	snprintf(r->host, sizeof(r->host), "%s", s->cfg.usb ? "127.0.0.1" : s->cfg.host);
	r->port = s->cfg.port;
	snprintf(r->key, sizeof(r->key), "%s", s->cfg.key);
	LeaveCriticalSection(&s->lock);
	snprintf(r->path, sizeof(r->path), "/api/set?%s", query);
	if (!*r->host) {
		bfree(r);
		return;
	}
	if (!mc_thread_detached(control_thread, r))
		bfree(r);
}

#ifdef _WIN32
obs_source_t *mc_obs_source(struct mc_source *s)
{
	return s->source;
}

void mc_get_phone_img(struct mc_source *s, struct mc_phone_img *out)
{
	EnterCriticalSection(&s->lock);
	*out = s->ph;
	out->connected = s->connected;
	LeaveCriticalSection(&s->lock);
}

void mc_send_control_query(struct mc_source *s, const char *query)
{
	send_control(s, query);
}

const char *mc_phone_version(void)
{
	return MC_PHONE_VERSION;
}
#endif

static const char *mc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "MagerCam (телефон)";
}

static void mc_defaults(obs_data_t *st)
{
	obs_data_set_default_string(st, "host", "");
	obs_data_set_default_int(st, "port", DEFAULT_PORT);
	obs_data_set_default_int(st, "conn", 0);
	obs_data_set_default_string(st, "adb_path", "");
	obs_data_set_default_string(st, "resolution", "1920x1080");
	obs_data_set_default_int(st, "fps", 30);
	obs_data_set_default_int(st, "bitrate", 8);
	/* без сохранённого значения и у старых источников со слайдером тоже авто: так задумано */
	obs_data_set_default_bool(st, "bitrate_auto", true);
	obs_data_set_default_bool(st, "quality_mode", false);
	obs_data_set_default_bool(st, "audio", true);
	obs_data_set_default_int(st, "camera", 0);
	obs_data_set_default_string(st, "camera_id", "");
	obs_data_set_default_int(st, "rotation", -1);
	obs_data_set_default_bool(st, "mirror", false);
	obs_data_set_default_bool(st, "low_latency", true);
	/* 4K в HEVC процессор декодирует впритык; если видеокарта не умеет, сам откачусь на программное */
	obs_data_set_default_bool(st, "hw_decode", true);
	obs_data_set_default_bool(st, "deactivate_hidden", false);
	obs_data_set_default_bool(st, "clear_on_disconnect", false);
	obs_data_set_default_bool(st, "control", true);
	obs_data_set_default_double(st, "zoom", 1.0);
	obs_data_set_default_double(st, "ev", 0.0);
	obs_data_set_default_string(st, "wb", "auto");
	obs_data_set_default_int(st, "wb_temp", 5000);
	obs_data_set_default_int(st, "wb_tint", 0);
	obs_data_set_default_string(st, "focus", "continuous");
	obs_data_set_default_string(st, "antibanding", "auto");
	obs_data_set_default_bool(st, "stab", false);
	obs_data_set_default_string(st, "key", "");
	obs_data_set_default_bool(st, "hevc", true);
	obs_data_set_default_string(st, "profile", "");
	obs_data_set_default_string(st, "bg", "off");
	obs_data_set_default_double(st, "bg_strength", 0.6);
	obs_data_set_default_bool(st, "framing", false);
	obs_data_set_default_bool(st, "lock", false);
	obs_data_set_default_string(st, "sharp", "default");
	obs_data_set_default_string(st, "nr", "default");
	obs_data_set_default_int(st, "sharp_amount", 0);
}

static void read_config(obs_data_t *st, struct mc_config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", obs_data_get_string(st, "host"));
	char *h = c->host;
	while (*h == ' ')
		memmove(h, h + 1, strlen(h));
	for (size_t n = strlen(h); n && h[n - 1] == ' '; n--)
		h[n - 1] = 0;
	c->port = (int)obs_data_get_int(st, "port");
	if (c->port <= 0 || c->port > 65535)
		c->port = DEFAULT_PORT;
	c->usb = obs_data_get_int(st, "conn") == 1;
	snprintf(c->adb_path, sizeof(c->adb_path), "%s", obs_data_get_string(st, "adb_path"));
	if (sscanf(obs_data_get_string(st, "resolution"), "%dx%d", &c->width, &c->height) != 2) {
		c->width = 1920;
		c->height = 1080;
	}
	c->fps = (int)obs_data_get_int(st, "fps");
	c->bitrate_kbps = (int)obs_data_get_int(st, "bitrate") * 1000;
	c->bitrate_auto = obs_data_get_bool(st, "bitrate_auto");
	c->quality = obs_data_get_bool(st, "quality_mode");
	c->audio = obs_data_get_bool(st, "audio");
	const char *cam = obs_data_get_string(st, "camera_id");
	if (!*cam) /* старые версии хранили камеру числом */
		cam = obs_data_get_int(st, "camera") == 1 ? "front" : "back";
	snprintf(c->camera, sizeof(c->camera), "%s", cam);
	c->hw_decode = obs_data_get_bool(st, "hw_decode");
	c->control = obs_data_get_bool(st, "control");
	c->zoom = obs_data_get_double(st, "zoom");
	c->ev = obs_data_get_double(st, "ev");
	snprintf(c->wb, sizeof(c->wb), "%s", obs_data_get_string(st, "wb"));
	c->wb_temp = (int)obs_data_get_int(st, "wb_temp");
	c->wb_tint = (int)obs_data_get_int(st, "wb_tint");
	snprintf(c->focus, sizeof(c->focus), "%s", obs_data_get_string(st, "focus"));
	snprintf(c->antibanding, sizeof(c->antibanding), "%s", obs_data_get_string(st, "antibanding"));
	c->stab = obs_data_get_bool(st, "stab");
	snprintf(c->key, sizeof(c->key), "%s", obs_data_get_string(st, "key"));
	c->hevc = obs_data_get_bool(st, "hevc");
	snprintf(c->profile, sizeof(c->profile), "%s", obs_data_get_string(st, "profile"));
	snprintf(c->bg, sizeof(c->bg), "%s", obs_data_get_string(st, "bg"));
	c->bg_strength = obs_data_get_double(st, "bg_strength");
	c->framing = obs_data_get_bool(st, "framing");
	c->lock = obs_data_get_bool(st, "lock");
	/* в запрос значения идут как есть, поэтому беру только известные */
	static const char *const levels[] = {"off", "low", "default", "high"};
	const char *sh = obs_data_get_string(st, "sharp"), *nr = obs_data_get_string(st, "nr");
	int amount = (int)obs_data_get_int(st, "sharp_amount");
	c->sharp_amount = amount < -100 ? -100 : amount > 100 ? 100 : amount;
	snprintf(c->sharp, sizeof(c->sharp), "default");
	snprintf(c->nr, sizeof(c->nr), "default");
	for (size_t i = 0; i < sizeof(levels) / sizeof(levels[0]); i++) {
		if (strcmp(sh, levels[i]) == 0)
			snprintf(c->sharp, sizeof(c->sharp), "%s", levels[i]);
		if (strcmp(nr, levels[i]) == 0)
			snprintf(c->nr, sizeof(c->nr), "%s", levels[i]);
	}
}

static bool stream_params_differ(const struct mc_config *a, const struct mc_config *b)
{
	return strcmp(a->host, b->host) != 0 || a->port != b->port || a->usb != b->usb ||
	       strcmp(a->adb_path, b->adb_path) != 0 || a->width != b->width || a->height != b->height ||
	       a->fps != b->fps || a->audio != b->audio || strcmp(a->camera, b->camera) != 0 || a->hw_decode != b->hw_decode ||
	       strcmp(a->key, b->key) != 0 || a->hevc != b->hevc;
}

/* то, что задаёт профиль (см. profileParams в Core.kt) */
static bool profile_fields_differ(const struct mc_config *a, const struct mc_config *b)
{
	return a->width != b->width || a->height != b->height || a->fps != b->fps || a->bitrate_auto != b->bitrate_auto ||
	       (!a->bitrate_auto && a->bitrate_kbps != b->bitrate_kbps) ||
	       strcmp(a->camera, b->camera) != 0 || a->framing != b->framing || a->stab != b->stab ||
	       strcmp(a->focus, b->focus) != 0 || strcmp(a->bg, b->bg) != 0;
}

static void mc_update(void *data, obs_data_t *st)
{
	struct mc_source *s = data;
	struct mc_config c;
	read_config(st, &c);

	EnterCriticalSection(&s->lock);
	struct mc_config old = s->cfg;
	bool first = !s->initialized;
	if (first || strcmp(old.profile, c.profile) != 0) {
		if (c.profile[0])
			snprintf(s->profile_live, sizeof(s->profile_live), "%s", c.profile);
	} else if (profile_fields_differ(&old, &c)) {
		/* правка руками отменяет профиль, иначе при переподключении он вернул бы всё назад */
		s->profile_live[0] = 0;
		if (c.profile[0]) {
			c.profile[0] = 0;
			obs_data_set_string(st, "profile", "");
		}
	}
	s->cfg = c;
	s->initialized = true;
	s->rotation = (int)obs_data_get_int(st, "rotation");
	s->mirror = obs_data_get_bool(st, "mirror");
	s->low_latency = obs_data_get_bool(st, "low_latency");
	s->deactivate_hidden = obs_data_get_bool(st, "deactivate_hidden");
	s->clear_on_disconnect = obs_data_get_bool(st, "clear_on_disconnect");
	bool connected = s->connected;
	LeaveCriticalSection(&s->lock);

	obs_source_set_async_unbuffered(s->source, s->low_latency);

	if (first)
		return;

	if (stream_params_differ(&old, &c)) {
		force_reconnect(s);
		return;
	}

	mc_event_set(s->wake); /* deactivate_hidden мог поменяться */

	if (!connected)
		return;

	struct dstr q = {0};
	if (old.bitrate_auto != c.bitrate_auto || (!c.bitrate_auto && old.bitrate_kbps != c.bitrate_kbps)) {
		if (c.bitrate_auto)
			dstr_cat(&q, "&bitrate=auto");
		else
			dstr_catf(&q, "&bitrate=%d", c.bitrate_kbps);
	}
	if (old.quality != c.quality)
		dstr_catf(&q, "&enc=%s", c.quality ? "quality" : "latency");
	if (c.control) {
		if (old.zoom != c.zoom || !old.control)
			dstr_catf(&q, "&zoom=%.2f", c.zoom);
		if (old.ev != c.ev || !old.control)
			dstr_catf(&q, "&ev=%.2f", c.ev);
		bool wb_changed = strcmp(old.wb, c.wb) != 0 || !old.control;
		if (wb_changed) {
			dstr_cat(&q, "&wb=");
			url_encode(&q, c.wb);
		}
		if (strcmp(c.wb, "manual") == 0) {
			if (wb_changed || old.wb_temp != c.wb_temp)
				dstr_catf(&q, "&wb_temp=%d", c.wb_temp);
			if (wb_changed || old.wb_tint != c.wb_tint)
				dstr_catf(&q, "&wb_tint=%d", c.wb_tint);
		}
		if (strcmp(old.focus, c.focus) != 0 || !old.control) {
			dstr_cat(&q, "&focus=");
			url_encode(&q, c.focus);
		}
		if (strcmp(old.antibanding, c.antibanding) != 0 || !old.control) {
			dstr_cat(&q, "&antibanding=");
			url_encode(&q, c.antibanding);
		}
		if (old.stab != c.stab || !old.control)
			dstr_catf(&q, "&stab=%d", c.stab ? 1 : 0);
		if (strcmp(old.sharp, c.sharp) != 0 || !old.control)
			dstr_catf(&q, "&sharp=%s", c.sharp);
		if (strcmp(old.nr, c.nr) != 0 || !old.control)
			dstr_catf(&q, "&nr=%s", c.nr);
		if (old.sharp_amount != c.sharp_amount || !old.control)
			dstr_catf(&q, "&sharp_amount=%d", c.sharp_amount);
		if (old.framing != c.framing || !old.control)
			dstr_catf(&q, "&framing=%d", c.framing ? 1 : 0);
		if (old.lock != c.lock || !old.control)
			dstr_catf(&q, "&lock=%d", c.lock ? 1 : 0);
		if (strcmp(old.bg, c.bg) != 0 || !old.control)
			dstr_catf(&q, "&bg=%s", c.bg);
		if (old.bg_strength != c.bg_strength || !old.control)
			dstr_catf(&q, "&bg_strength=%.2f", c.bg_strength);
		/* профиль шлю только при смене: он сам переставит разрешение, камеру и прочее */
		if (strcmp(old.profile, c.profile) != 0 && c.profile[0])
			dstr_catf(&q, "&profile=%s", c.profile);
	}
	if (q.len)
		send_control(s, q.array + 1);
	dstr_free(&q);
}

static void *mc_create(obs_data_t *st, obs_source_t *source)
{
	struct mc_source *s = bzalloc(sizeof(*s));
	s->source = source;
	s->sock = INVALID_SOCKET;
	s->rotation = -1;
	InitializeCriticalSection(&s->lock);
	s->wake = mc_event_create();
	snprintf(s->status, sizeof(s->status), "Запуск…");
	s->pacer = pacer_create(s);
#ifdef MC_FRAME_PROBE
	s->probe = bzalloc(sizeof(*s->probe));
	s->probe->s = s;
	s->probe->no_pacer = getenv("MAGERCAM_NO_PACER") != NULL;
	obs_add_raw_video_callback(NULL, probe_render, s->probe);
	LOG(LOG_INFO, "frame probe on, pacer %s", s->probe->no_pacer ? "disabled" : "enabled");
#endif
	mc_update(s, st);
	s->showing = obs_source_showing(source);
	s->has_thread = mc_thread_start(&s->thread, net_thread, s);
	return s;
}

static void mc_destroy(void *data)
{
	struct mc_source *s = data;
#ifdef _WIN32
	imgwin_source_gone(s);
#endif
	s->stop = true;
	force_reconnect(s);
	if (s->has_thread)
		mc_thread_join(s->thread);
	pacer_destroy(s->pacer);
#ifdef MC_FRAME_PROBE
	obs_remove_raw_video_callback(probe_render, s->probe);
	bfree(s->probe->buf);
	bfree(s->probe);
#endif
	mc_event_destroy(s->wake);
	DeleteCriticalSection(&s->lock);
	bfree(s->rot_buf);
	bfree(s->payload);
	obs_data_array_release(s->cameras);
	bfree(s);
}

static void mc_show(void *data)
{
	struct mc_source *s = data;
	s->showing = true;
	mc_event_set(s->wake);
}

static void mc_hide(void *data)
{
	struct mc_source *s = data;
	s->showing = false;
	EnterCriticalSection(&s->lock);
	bool deact = s->deactivate_hidden;
	LeaveCriticalSection(&s->lock);
	if (deact)
		force_reconnect(s);
}

/* Значение списка всегда сбрасываю в "", иначе при следующем открытии свойств старый выбор затёр бы
 * IP, вписанный руками. Поэтому выбранный телефон показываю текстом первой строки. */
static void fill_device_list(obs_property_t *list, const char *host)
{
	obs_property_list_clear(list);
	char first[200] = "— выберите найденный телефон —";
	EnterCriticalSection(&g_found_lock);
	for (int i = 0; i < g_found_count; i++)
		if (host && *host && strcmp(g_found[i].ip, host) == 0)
			snprintf(first, sizeof(first), "✓ %s (%s)", g_found[i].name, g_found[i].ip);
	obs_property_list_add_string(list, first, "");
	for (int i = 0; i < g_found_count; i++) {
		char label[200], value[96];
		snprintf(label, sizeof(label), "%s (%s)%s", g_found[i].name, g_found[i].ip,
			 g_found[i].auth ? " — нужен код" : "");
		snprintf(value, sizeof(value), "%s:%d", g_found[i].ip, g_found[i].port);
		obs_property_list_add_string(list, label, value);
	}
	if (g_found_count == 0)
		obs_property_list_add_string(list, "(телефоны не найдены — нажмите «Найти»)", "");
	LeaveCriticalSection(&g_found_lock);
}

static bool on_device_selected(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *st)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(props);
	const char *v = obs_data_get_string(st, "device");
	if (!v || !*v)
		return false;
	char ip[64];
	int port = DEFAULT_PORT;
	if (sscanf(v, "%63[^:]:%d", ip, &port) >= 1) {
		obs_data_set_string(st, "host", ip);
		obs_data_set_int(st, "port", port);
		obs_data_set_int(st, "conn", 0);
		fill_device_list(p, ip);
	}
	obs_data_set_string(st, "device", "");
	return true;
}

static bool on_conn_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *st)
{
	UNUSED_PARAMETER(p);
	bool usb = obs_data_get_int(st, "conn") == 1;
	obs_property_set_visible(obs_properties_get(props, "host"), !usb);
	obs_property_set_visible(obs_properties_get(props, "device"), !usb);
	obs_property_set_visible(obs_properties_get(props, "discover"), !usb);
	obs_property_set_visible(obs_properties_get(props, "adb_path"), usb);
	return true;
}

static bool on_bitrate_auto_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *st)
{
	UNUSED_PARAMETER(p);
	obs_property_set_visible(obs_properties_get(props, "bitrate"), !obs_data_get_bool(st, "bitrate_auto"));
	return true;
}

static bool on_discover(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	struct mc_source *s = data;
	discovery_run(1500);
	char host[128] = "";
	if (s) {
		EnterCriticalSection(&s->lock);
		snprintf(host, sizeof(host), "%s", s->cfg.host);
		LeaveCriticalSection(&s->lock);
	}
	fill_device_list(obs_properties_get(props, "device"), host);
	return true;
}

static bool on_reconnect(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	force_reconnect(data);
	return false;
}

static bool on_switch_camera(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct mc_source *s = data;
	obs_data_t *st = obs_source_get_settings(s->source);
	char cur[64];
	EnterCriticalSection(&s->lock);
	snprintf(cur, sizeof(cur), "%s", s->cfg.camera);
	/* переключаю на следующий объектив из списка телефона, а пока он его не прислал — просто back/front */
	const char *next = strcmp(cur, "front") == 0 ? "back" : "front";
	char buf[64];
	size_t n = s->cameras ? obs_data_array_count(s->cameras) : 0;
	for (size_t i = 0; i < n; i++) {
		obs_data_t *c = obs_data_array_item(s->cameras, i);
		bool match = strcmp(obs_data_get_string(c, "id"), cur) == 0;
		obs_data_release(c);
		if (match || i == n - 1) {
			obs_data_t *nc = obs_data_array_item(s->cameras, match ? (i + 1) % n : 0);
			snprintf(buf, sizeof(buf), "%s", obs_data_get_string(nc, "id"));
			obs_data_release(nc);
			next = buf;
			break;
		}
	}
	LeaveCriticalSection(&s->lock);
	obs_data_set_string(st, "camera_id", next);
	obs_source_update(s->source, st);
	obs_data_release(st);
	return true;
}

/* выдержку в саму строку статуса не кладу: она меняется каждую секунду, а каждая смена статуса пишется в лог */
static void format_status(struct mc_source *s, char *buf, size_t sz)
{
	EnterCriticalSection(&s->lock);
	char expo[96];
	bool dark = false;
	bool have = s->connected && mc_format_exposure(&s->ph, expo, sizeof(expo), &dark);
	size_t o = (size_t)snprintf(buf, sz, "Статус: %s", s->status);
	if (have && o < sz)
		o += (size_t)snprintf(buf + o, sz - o, "\n%s%s", expo, dark ? " — " MC_DARK_HINT : "");
	if (s->hint[0] && o < sz)
		snprintf(buf + o, sz - o, "\n⚠ %s", s->hint);
	LeaveCriticalSection(&s->lock);
}

static bool on_refresh_status(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	char status[800];
	format_status(data, status, sizeof(status));
	obs_property_set_description(obs_properties_get(props, "status"), status);
	return true;
}

#ifdef _WIN32
static bool on_image_settings(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	imgwin_open(data);
	return false;
}
#else
/* В Windows это всё в отдельном окне, а здесь — в группе, которая раскрывается галочкой: свёрнутая группа
 * не загромождает подключение. Сворачивать группы OBS не умеет, поэтому содержимое прячу сам. */
static const char *const image_keys[] = {"expo", "control", "torch", "autofocus", "zoom", "ev", "focus", "wb", "wb_temp",
					 "wb_tint", "antibanding", "sharp", "sharp_amount", "nr", "stab", "lock", "profile", "framing",
					 "bg", "bg_strength", "record", "rotation", "mirror"};

static void sync_image_group(obs_properties_t *props, obs_data_t *st)
{
	bool open = obs_data_get_bool(st, "grp_image");
	bool manual = strcmp(obs_data_get_string(st, "wb"), "manual") == 0;
	for (size_t i = 0; i < sizeof(image_keys) / sizeof(image_keys[0]); i++) {
		bool wb_part = strcmp(image_keys[i], "wb_temp") == 0 || strcmp(image_keys[i], "wb_tint") == 0;
		obs_property_set_visible(obs_properties_get(props, image_keys[i]), open && (!wb_part || manual));
	}
}

static bool on_image_group_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *st)
{
	UNUSED_PARAMETER(p);
	sync_image_group(props, st);
	return true;
}

static bool on_torch(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	send_control(data, "torch=toggle");
	return false;
}

static bool on_record(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct mc_source *s = data;
	EnterCriticalSection(&s->lock);
	bool rec = s->phone_rec;
	LeaveCriticalSection(&s->lock);
	send_control(s, rec ? "rec=0" : "rec=1");
	return false;
}

static bool on_autofocus(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	send_control(data, "af=trigger&x=0.5&y=0.5");
	return false;
}

static void add_levels(obs_property_t *p)
{
	obs_property_list_add_string(p, "Выкл", "off");
	obs_property_list_add_string(p, "Слабая", "low");
	obs_property_list_add_string(p, "Стандарт (как у камеры)", "default");
	obs_property_list_add_string(p, "Сильная", "high");
}

static void add_image_group(obs_properties_t *props, struct mc_source *s)
{
	obs_property_t *p;
	obs_properties_t *img = obs_properties_create();
	char expo[200] = "Выдержка и ISO — после подключения к телефону";
	if (s) {
		char e[96];
		bool dark = false;
		EnterCriticalSection(&s->lock);
		if (s->connected && mc_format_exposure(&s->ph, e, sizeof(e), &dark))
			snprintf(expo, sizeof(expo), "%s%s", e, dark ? " — " MC_DARK_HINT : "");
		LeaveCriticalSection(&s->lock);
	}
	obs_properties_add_text(img, "expo", expo, OBS_TEXT_INFO);
	obs_properties_add_bool(img, "control", "Управлять камерой из OBS");
	obs_properties_add_button(img, "torch", "Фонарик вкл/выкл", on_torch);
	obs_properties_add_button(img, "autofocus", "Сфокусироваться (по центру)", on_autofocus);
	p = obs_properties_add_list(img, "profile", "Профиль", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Свои настройки", "");
	obs_property_list_add_string(p, "Звонок (720p, следить за лицом)", "call");
	obs_property_list_add_string(p, "Стрим (1080p60)", "stream");
	obs_property_list_add_string(p, "Документ", "doc");
	obs_properties_add_float_slider(img, "zoom", "Зум", 1.0, 10.0, 0.1);
	obs_properties_add_float_slider(img, "ev", "Экспозиция (EV)", -3.0, 3.0, 0.1);
	p = obs_properties_add_list(img, "focus", "Фокус", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Непрерывный автофокус", "continuous");
	obs_property_list_add_string(p, "Автофокус по кнопке", "auto");
	obs_property_list_add_string(p, "Бесконечность", "infinity");
	obs_property_list_add_string(p, "Макро", "macro");
	p = obs_properties_add_list(img, "wb", "Баланс белого", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Авто", "auto");
	obs_property_list_add_string(p, "Дневной свет", "daylight");
	obs_property_list_add_string(p, "Облачно", "cloudy");
	obs_property_list_add_string(p, "Тень", "shade");
	obs_property_list_add_string(p, "Лампа накаливания", "incandescent");
	obs_property_list_add_string(p, "Флуоресцентная лампа", "fluorescent");
	obs_property_list_add_string(p, "Сумерки", "twilight");
	obs_property_list_add_string(p, "Вручную (температура)", "manual");
	obs_property_set_modified_callback(p, on_image_group_changed);
	p = obs_properties_add_int_slider(img, "wb_temp", "Температура, K", 2000, 10000, 100);
	obs_property_set_long_description(p, "Температура света, который должен стать белым: под лампой 3200 K ставьте "
					     "3200. Меньше — картинка холоднее, больше — теплее.");
	obs_properties_add_int_slider(img, "wb_tint", "Оттенок (− зелень, + пурпур)", -50, 50, 1);
	p = obs_properties_add_list(img, "antibanding", "Антимерцание", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Авто", "auto");
	obs_property_list_add_string(p, "50 Гц (Россия/Европа)", "50");
	obs_property_list_add_string(p, "60 Гц", "60");
	obs_property_list_add_string(p, "Выкл", "off");
	p = obs_properties_add_list(img, "sharp", "Резкость камеры", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_levels(p);
	obs_property_set_long_description(p, "Обработка камеры телефона. Если вокруг контуров светлые ореолы, ставьте «Слабая» "
					     "или «Выкл». Уровень, которого камера не умеет, телефон не применит.");
	p = obs_properties_add_int_slider(img, "sharp_amount", "Доп. резкость (− мягче, + резче)", -100, 100, 1);
	obs_property_set_long_description(p, "Для мягкой картинки: камера — Выкл, ползунок по вкусу.");
	p = obs_properties_add_list(img, "nr", "Шумоподавление", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	add_levels(p);
	obs_property_set_long_description(p, "При слабом свете сильное шумоподавление делает кожу «восковой»: тогда «Слабая».");
	obs_properties_add_bool(img, "stab", "Стабилизация видео");
	obs_properties_add_bool(img, "lock", "Зафиксировать экспозицию и баланс белого");
	obs_properties_add_bool(img, "framing", "Следить за лицом (автокадрирование)");
	p = obs_properties_add_list(img, "bg", "Фон", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Без обработки", "off");
	obs_property_list_add_string(p, "Размыть", "blur");
	obs_property_list_add_string(p, "Заменить картинкой (выбирается на телефоне или в программе для Windows)", "image");
	obs_properties_add_float_slider(img, "bg_strength", "Сила размытия", 0.0, 1.0, 0.05);
	obs_properties_add_button(img, "record", "Запись на телефоне: старт/стоп", on_record);
	p = obs_properties_add_list(img, "rotation", "Поворот", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Авто (по положению телефона)", -1);
	obs_property_list_add_int(p, "0°", 0);
	obs_property_list_add_int(p, "90°", 90);
	obs_property_list_add_int(p, "180°", 180);
	obs_property_list_add_int(p, "270°", 270);
	obs_properties_add_bool(img, "mirror", "Отразить по горизонтали");
	p = obs_properties_add_group(props, "grp_image", "Настройки изображения (галочка — показать)", OBS_GROUP_CHECKABLE,
				     img);
	obs_property_set_modified_callback(p, on_image_group_changed);
}
#endif

static obs_properties_t *mc_properties(void *data)
{
	struct mc_source *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	char status[800] = "Статус: —";
	if (s)
		format_status(s, status, sizeof(status));
	p = obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
	obs_properties_add_button(props, "refresh_status", "Обновить статус", on_refresh_status);
#ifdef _WIN32
	p = obs_properties_add_button(props, "image_settings", "Настройки изображения…", on_image_settings);
	obs_property_set_long_description(p, "Зум, экспозиция, баланс белого, резкость, фон, поворот и прочее — в отдельном "
					     "окне, которое можно держать открытым рядом с OBS.");
#endif

	obs_properties_t *conn = obs_properties_create();
	p = obs_properties_add_list(conn, "conn", "Подключение", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Wi-Fi / локальная сеть", 0);
	obs_property_list_add_int(p, "USB (через ADB)", 1);
	obs_property_set_modified_callback(p, on_conn_changed);

	p = obs_properties_add_list(conn, "device", "Найденные телефоны", OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	if (g_found_count == 0)
		discovery_run(800);
	char cur_host[128] = "";
	if (s) {
		EnterCriticalSection(&s->lock);
		snprintf(cur_host, sizeof(cur_host), "%s", s->cfg.host);
		LeaveCriticalSection(&s->lock);
	}
	fill_device_list(p, cur_host);
	obs_property_set_modified_callback2(p, on_device_selected, s);
	obs_properties_add_button(conn, "discover", "Найти телефоны в сети", on_discover);
	obs_properties_add_text(conn, "host", "IP телефона", OBS_TEXT_DEFAULT);
	obs_properties_add_int(conn, "port", "Порт", 1, 65535, 1);
	p = obs_properties_add_path(conn, "adb_path", "Путь к adb.exe (необязательно)", OBS_PATH_FILE, "adb.exe (adb.exe)",
				    NULL);
	p = obs_properties_add_text(conn, "key", "Код доступа", OBS_TEXT_PASSWORD);
	obs_property_set_long_description(p, "Шестизначный код с экрана телефона. По USB он не нужен.");
	obs_properties_add_bool(conn, "hevc", "H.265, если телефон умеет (меньше трафика)");
	obs_properties_add_button(conn, "reconnect", "Переподключиться", on_reconnect);
	obs_properties_add_group(props, "grp_conn", "Подключение", OBS_GROUP_NORMAL, conn);

	obs_properties_t *vid = obs_properties_create();
	/* после подключения списки берутся у телефона, до него показываю типовые */
	int sizes[32][2], ns = 0, fps[8], nf = 0;
	if (s) {
		EnterCriticalSection(&s->lock);
		ns = s->phone_nsizes;
		memcpy(sizes, s->phone_sizes, sizeof(sizes));
		nf = s->phone_nfps;
		memcpy(fps, s->phone_fps, sizeof(fps));
		LeaveCriticalSection(&s->lock);
	}
	if (!ns) {
		static const int def[][2] = {{3840, 2160}, {2560, 1440}, {1920, 1080}, {1280, 720}, {640, 480}};
		memcpy(sizes, def, sizeof(def));
		ns = 5;
	}
	if (!nf) {
		static const int def[] = {24, 25, 30, 50, 60};
		memcpy(fps, def, sizeof(def));
		nf = 5;
	}
	p = obs_properties_add_list(vid, "resolution", "Разрешение", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (int i = 0; i < ns; i++) {
		int w = sizes[i][0], h = sizes[i][1];
		const char *tag = w == 3840 && h == 2160 ? " (4K)"
				  : w == 2560 && h == 1440 ? " (2K)"
				  : w == 1920 && h == 1080 ? " (Full HD)"
				  : w == 1280 && h == 720  ? " (HD)"
							   : "";
		char val[24], name[48];
		snprintf(val, sizeof(val), "%dx%d", w, h);
		snprintf(name, sizeof(name), "%dx%d%s", w, h, tag);
		obs_property_list_add_string(p, name, val);
	}
	p = obs_properties_add_list(vid, "fps", "Кадров в секунду", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < nf; i++) {
		char name[8];
		snprintf(name, sizeof(name), "%d", fps[i]);
		obs_property_list_add_int(p, name, fps[i]);
	}
	p = obs_properties_add_bool(vid, "bitrate_auto", "Подбирать битрейт автоматически");
	obs_property_set_long_description(p, "Телефон сам выберет битрейт под разрешение, частоту кадров и кодек "
					     "(например, 1080p 30 — около 18 Мбит/с). Адаптивный битрейт при плохой сети "
					     "по-прежнему снижает его.");
	obs_property_set_modified_callback(p, on_bitrate_auto_changed);
	p = obs_properties_add_int_slider(vid, "bitrate", "Битрейт (Мбит/с)", 1, 50, 1);
	p = obs_properties_add_bool(vid, "quality_mode", "Режим «Качество» (лучше детализация, задержка +1–2 кадра)");
	obs_property_set_long_description(p, "Телефон кодирует с переменным битрейтом и B-кадрами, если его кодер их умеет. "
					     "Режим общий для всех, кто смотрит этот телефон.");
	p = obs_properties_add_list(vid, "camera_id", "Камера", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Задняя (основная)", "back");
	obs_property_list_add_string(p, "Фронтальная", "front");
	if (s) {
		EnterCriticalSection(&s->lock);
		size_t n = s->cameras ? obs_data_array_count(s->cameras) : 0;
		for (size_t i = 0; i < n; i++) {
			obs_data_t *c = obs_data_array_item(s->cameras, i);
			obs_property_list_add_string(p, obs_data_get_string(c, "label"), obs_data_get_string(c, "id"));
			obs_data_release(c);
		}
		LeaveCriticalSection(&s->lock);
		if (n == 0)
			obs_property_set_long_description(
				p, "Остальные объективы (широкоугольный, телефото) появятся после подключения к телефону");
	}
	obs_properties_add_button(vid, "switch_camera", "Сменить камеру", on_switch_camera);
	obs_properties_add_bool(vid, "audio", "Передавать звук с микрофона телефона");
	obs_properties_add_group(props, "grp_video", "Видео и звук", OBS_GROUP_NORMAL, vid);

#ifndef _WIN32
	add_image_group(props, s);
#endif

	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_bool(adv, "low_latency", "Минимальная задержка (без буферизации)");
	obs_properties_add_bool(adv, "hw_decode", MC_HW_LABEL);
	obs_properties_add_bool(adv, "deactivate_hidden", "Отключаться, когда источник не виден");
	obs_properties_add_bool(adv, "clear_on_disconnect", "Очищать картинку при потере связи");
	obs_properties_add_group(props, "grp_adv", "Дополнительно", OBS_GROUP_NORMAL, adv);

	p = obs_properties_add_text(props, "about", "MagerCam " PLUGIN_VERSION " — веб-панель телефона: http://IP:4848/",
				    OBS_TEXT_INFO);
	return props;
}

static struct obs_source_info magercam_source = {
	.id = "magercam_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,
	.get_name = mc_get_name,
	.create = mc_create,
	.destroy = mc_destroy,
	.update = mc_update,
	.get_defaults = mc_defaults,
	.get_properties = mc_properties,
	.show = mc_show,
	.hide = mc_hide,
	.icon_type = OBS_ICON_TYPE_CAMERA,
};

bool obs_module_load(void)
{
#ifdef _WIN32
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
	InitializeCriticalSection(&g_found_lock);
#ifdef _WIN32
	imgwin_init();
	/* сначала то, что OBS уже загрузил сам, иначе ищу рядом с obs64.exe; новые версии вперёд */
	const struct mc_ffapi *(*const variants[])(bool) = {mc_ff8_api, mc_ff7_api, mc_ff6_api};
	for (int pass = 0; pass < 2 && !g_ff; pass++)
		for (size_t i = 0; i < sizeof(variants) / sizeof(variants[0]) && !g_ff; i++)
			g_ff = variants[i](pass == 1);
#else
	g_ff = mc_ff_api(true);
#endif
	/* источник регистрирую и без FFmpeg: тогда он покажет понятную ошибку, а не пропадёт из списка */
	obs_register_source(&magercam_source);
	LOG(LOG_INFO, "plugin loaded, version " PLUGIN_VERSION ", decoder %s", g_ff ? g_ff->name : "not found");
	return true;
}

void obs_module_unload(void)
{
	DeleteCriticalSection(&g_found_lock);
#ifdef _WIN32
	imgwin_free();
	WSACleanup();
#endif
}

const char *obs_module_name(void)
{
	return "MagerCam";
}

const char *obs_module_description(void)
{
	return "Телефон как камера для OBS (MagerCam)";
}
