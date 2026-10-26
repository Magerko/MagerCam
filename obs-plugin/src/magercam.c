/*
 * MagerCam OBS plugin — телефон как камера для OBS.
 * Принимает H.264 + PCM с приложения MagerCam (см. docs/PROTOCOL.md) по Wi-Fi или USB (adb forward).
 */
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <obs-module.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <util/bmem.h>
/* from util/threading.h (header itself needs MSVC intrinsics) */
EXPORT void os_set_thread_name(const char *name);

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>

OBS_DECLARE_MODULE()

#define PLUGIN_VERSION "1.0.0"
#define LOG(level, fmt, ...) blog(level, "[MagerCam] " fmt, ##__VA_ARGS__)

#define DEFAULT_PORT 4848
#define DISCOVERY_PORT 4849

enum pkt_type {
	PKT_VIDEO_CONFIG = 1,
	PKT_VIDEO_FRAME = 2,
	PKT_AUDIO_CONFIG = 3,
	PKT_AUDIO = 4,
	PKT_STATUS = 5,
	PKT_PING = 6,
};

/* ------------------------------------------------------------------------- */
/* Discovery results (shared by all sources)                                 */

struct found_device {
	char name[96];
	char ip[64];
	int port;
};

#define MAX_FOUND 32
static struct found_device g_found[MAX_FOUND];
static int g_found_count;
static CRITICAL_SECTION g_found_lock;

/* ------------------------------------------------------------------------- */

struct mc_config {
	char host[128];
	int port;
	bool usb;
	char adb_path[512];
	int width, height, fps, bitrate_kbps;
	bool audio;
	char camera[64]; /* "back", "front" or a lens id from the phone status */
	bool hw_decode;
	bool control;
	double zoom, ev;
	char wb[32], focus[32], antibanding[16];
	bool stab;
};

struct mc_source {
	obs_source_t *source;

	CRITICAL_SECTION lock;
	struct mc_config cfg;
	int rotation; /* -1 = auto */
	bool mirror;
	bool low_latency;
	bool deactivate_hidden;
	bool clear_on_disconnect;
	bool initialized;

	volatile LONG gen;
	volatile bool stop;
	volatile bool showing;
	HANDLE thread;
	HANDLE wake;
	SOCKET sock;

	/* status from phone (guarded by lock) */
	char status[256];
	char phone_name[96];
	int phone_orient;
	int battery;
	bool connected;
	obs_data_array_t *cameras; /* lenses reported by the phone */

	/* net thread only */
	AVCodecContext *dec;
	AVBufferRef *hw_dev;
	AVPacket *pkt;
	AVFrame *frame;
	AVFrame *sw_frame;
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
};

/* ------------------------------------------------------------------------- */
/* Small utils                                                               */

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

static void set_status(struct mc_source *s, const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	EnterCriticalSection(&s->lock);
	if (strcmp(buf, s->status) != 0) {
		strncpy(s->status, buf, sizeof(s->status) - 1);
		LOG(LOG_INFO, "%s", buf);
	}
	LeaveCriticalSection(&s->lock);
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

/* ------------------------------------------------------------------------- */
/* Sockets                                                                   */

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

	u_long nb = 1;
	ioctlsocket(sock, FIONBIO, &nb);
	int r = connect(sock, res->ai_addr, (int)res->ai_addrlen);
	freeaddrinfo(res);
	if (r == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
		closesocket(sock);
		return INVALID_SOCKET;
	}

	fd_set wfds, efds;
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	FD_SET(sock, &wfds);
	FD_SET(sock, &efds);
	struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
	r = select(0, NULL, &wfds, &efds, &tv);
	if (r <= 0 || FD_ISSET(sock, &efds)) {
		closesocket(sock);
		return INVALID_SOCKET;
	}

	nb = 0;
	ioctlsocket(sock, FIONBIO, &nb);
	int one = 1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
	int rcvbuf = 4 * 1024 * 1024;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (const char *)&rcvbuf, sizeof(rcvbuf));
	return sock;
}

static void set_recv_timeout(SOCKET sock, int ms)
{
	DWORD t = (DWORD)ms;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
}

static bool recv_all(SOCKET sock, uint8_t *buf, size_t n)
{
	while (n > 0) {
		int r = recv(sock, (char *)buf, (int)(n > 1 << 20 ? 1 << 20 : n), 0);
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
		int r = send(sock, buf, (int)n, 0);
		if (r <= 0)
			return false;
		buf += r;
		n -= (size_t)r;
	}
	return true;
}

/* Reads HTTP response headers, returns status code or -1. */
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

/* Simple blocking GET, response body discarded. */
static bool http_get(const char *host, int port, const char *path)
{
	SOCKET sock = tcp_connect(host, port, 2000);
	if (sock == INVALID_SOCKET)
		return false;
	set_recv_timeout(sock, 2000);
	struct dstr req = {0};
	dstr_printf(&req, "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	bool ok = send_all(sock, req.array, req.len);
	dstr_free(&req);
	int code = ok ? read_http_header(sock) : -1;
	closesocket(sock);
	return code == 200;
}

/* ------------------------------------------------------------------------- */
/* ADB (USB mode)                                                            */

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
			/* drain the rest */
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

	/* installed with MagerCam for Windows */
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

/* Returns true when a forward to localhost:port is set up. */
static bool adb_prepare(struct mc_source *s, const char *adb_cfg, int port)
{
	char adb[1024];
	if (!find_adb(adb_cfg, adb, sizeof(adb))) {
		set_status(s, "USB: adb.exe не найден (укажите путь в настройках)");
		return false;
	}

	char out[4096];
	run_process(adb, "start-server", NULL, 0, 15000);
	if (!run_process(adb, "devices", out, sizeof(out), 5000)) {
		set_status(s, "USB: не удалось запустить adb");
		return false;
	}

	/* candidates in state "device": USB serials (no ':') first, then wireless */
	char cand[8][128];
	int ncand = 0;
	bool unauthorized = false;
	for (int pass = 0; pass < 2; pass++) {
		char copy[4096];
		snprintf(copy, sizeof(copy), "%s", out);
		char *ctx = NULL;
		for (char *line = strtok_s(copy, "\r\n", &ctx); line; line = strtok_s(NULL, "\r\n", &ctx)) {
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

	/* several devices: take the one where MagerCam is running */
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
	return true;
}

/* ------------------------------------------------------------------------- */
/* Discovery                                                                 */

static void discovery_run(int timeout_ms)
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return;
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
	struct sockaddr_in local = {0};
	local.sin_family = AF_INET;
	bind(sock, (struct sockaddr *)&local, sizeof(local));

	static const char msg[] = "MAGERCAM_DISCOVER";
	struct sockaddr_in dst = {0};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(DISCOVERY_PORT);
	dst.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));

	/* directed broadcast on every adapter: 255.255.255.255 goes out of one NIC only */
	ULONG size = 32 * 1024;
	IP_ADAPTER_ADDRESSES *addrs = bmalloc(size);
	if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
				 NULL, addrs, &size) == NO_ERROR) {
		for (IP_ADAPTER_ADDRESSES *a = addrs; a; a = a->Next) {
			if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = a->FirstUnicastAddress; u; u = u->Next) {
				struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
				ULONG prefix = u->OnLinkPrefixLength;
				if (prefix == 0 || prefix >= 32)
					continue;
				uint32_t ip = ntohl(sin->sin_addr.s_addr);
				uint32_t mask = prefix ? 0xFFFFFFFFu << (32 - prefix) : 0;
				dst.sin_addr.s_addr = htonl((ip & mask) | ~mask);
				sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
			}
		}
	}
	bfree(addrs);

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
		if (select(0, &fds, NULL, NULL, &tv) <= 0)
			break;
		char buf[1024];
		struct sockaddr_in from;
		int fromlen = sizeof(from);
		int r = recvfrom(sock, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fromlen);
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

/* ------------------------------------------------------------------------- */
/* Video decoding                                                            */

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == AV_PIX_FMT_D3D11)
			return *p;
	return avcodec_default_get_format(ctx, fmts);
}

static void decoder_close(struct mc_source *s)
{
	if (s->dec)
		avcodec_free_context(&s->dec);
	if (s->hw_dev)
		av_buffer_unref(&s->hw_dev);
}

static bool decoder_open(struct mc_source *s, bool hw)
{
	decoder_close(s);
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
	if (!codec) {
		set_status(s, "Ошибка: нет декодера H.264");
		return false;
	}
	s->dec = avcodec_alloc_context3(codec);
	s->dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
	s->dec->flags2 |= AV_CODEC_FLAG2_FAST;
	s->dec->thread_type = FF_THREAD_SLICE;
	s->dec->thread_count = 0;

	if (hw && !s->hw_failed) {
		if (av_hwdevice_ctx_create(&s->hw_dev, AV_HWDEVICE_TYPE_D3D11VA, NULL, NULL, 0) == 0) {
			s->dec->hw_device_ctx = av_buffer_ref(s->hw_dev);
			s->dec->get_format = get_hw_format;
			LOG(LOG_INFO, "hardware decoding (D3D11VA) enabled");
		} else {
			LOG(LOG_WARNING, "D3D11VA unavailable, using software decoding");
			s->hw_failed = true;
		}
	}

	if (avcodec_open2(s->dec, codec, NULL) < 0) {
		avcodec_free_context(&s->dec);
		set_status(s, "Ошибка: не удалось открыть декодер");
		return false;
	}
	s->need_key = true;
	return true;
}

/* Rotates/mirrors one 8-bit plane. src_step=2 extracts every second byte (NV12 UV). */
static void transform_plane(const uint8_t *src, int sstride, int src_step, int w, int h, uint8_t *dst, int rot,
			    bool mirror)
{
	int dw = (rot == 90 || rot == 270) ? h : w;
	/* dx = bx + x*ax + y*ay ; dy = by + x*cx + y*cy */
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

/* Maps phone pts (us, shared by audio and video) to OBS time. A small lead keeps network jitter
 * from producing past audio timestamps, which would grow OBS' global audio buffering. */
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

static void output_frame(struct mc_source *s, AVFrame *f, int rot, bool mirror)
{
	struct obs_source_frame2 out = {0};
	enum video_format fmt;
	switch (f->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		fmt = VIDEO_FORMAT_I420;
		break;
	case AV_PIX_FMT_NV12:
		fmt = VIDEO_FORMAT_NV12;
		break;
	default:
		LOG(LOG_WARNING, "unsupported pixel format %d", f->format);
		return;
	}

	bool full = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
	enum video_colorspace cs;
	switch (f->colorspace) {
	case AVCOL_SPC_BT709:
		cs = VIDEO_CS_709;
		break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		cs = VIDEO_CS_601;
		break;
	default:
		cs = f->height >= 720 || f->width >= 1280 ? VIDEO_CS_709 : VIDEO_CS_601;
		break;
	}
	out.range = full ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;

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
		/* rotated output is always I420, planes packed with stride == width */
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

	int64_t pts_us = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
	out.timestamp = map_timestamp(s, pts_us, false);
	obs_source_output_video2(s->source, &out);
	s->had_video = true;
}

static void decode_packet(struct mc_source *s, const uint8_t *data, size_t len, int64_t pts, bool key, int rot,
			  bool mirror)
{
	if (!s->dec)
		return;
	if (s->need_key && !key)
		return;
	s->need_key = false;

	s->pkt->data = (uint8_t *)data;
	s->pkt->size = (int)len;
	s->pkt->pts = pts;
	s->pkt->dts = pts;
	s->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;

	int r = avcodec_send_packet(s->dec, s->pkt);
	if (r < 0 && r != AVERROR(EAGAIN)) {
		s->need_key = true;
		return;
	}
	for (;;) {
		r = avcodec_receive_frame(s->dec, s->frame);
		if (r < 0)
			break;
		AVFrame *f = s->frame;
		if (f->format == AV_PIX_FMT_D3D11) {
			av_frame_unref(s->sw_frame);
			if (av_hwframe_transfer_data(s->sw_frame, f, 0) < 0) {
				LOG(LOG_WARNING, "hw transfer failed, switching to software decoding");
				s->hw_failed = true;
				av_frame_unref(f);
				decoder_open(s, false);
				return;
			}
			s->sw_frame->pts = f->pts;
			s->sw_frame->best_effort_timestamp = f->best_effort_timestamp;
			s->sw_frame->color_range = f->color_range;
			s->sw_frame->colorspace = f->colorspace;
			f = s->sw_frame;
		}
		output_frame(s, f, rot, mirror);
		av_frame_unref(s->frame);
	}
}

/* ------------------------------------------------------------------------- */
/* Network thread                                                            */

static void apply_status_json(struct mc_source *s, const char *json)
{
	obs_data_t *d = obs_data_create_from_json(json);
	if (!d)
		return;
	EnterCriticalSection(&s->lock);
	s->phone_orient = (int)obs_data_get_int(d, "orient");
	s->battery = (int)obs_data_get_int(d, "battery");
	snprintf(s->phone_name, sizeof(s->phone_name), "%s", obs_data_get_string(d, "name"));
	obs_data_array_t *cams = obs_data_get_array(d, "cameras");
	if (cams) {
		obs_data_array_release(s->cameras);
		s->cameras = cams;
	}
	LeaveCriticalSection(&s->lock);
	set_status(s, "Подключено: %s — %dx%d@%d, батарея %d%%%s", obs_data_get_string(d, "name"),
		   (int)obs_data_get_int(d, "w"), (int)obs_data_get_int(d, "h"), (int)obs_data_get_int(d, "fps"),
		   (int)obs_data_get_int(d, "battery"), obs_data_get_bool(d, "charging") ? " (заряжается)" : "");
	obs_data_release(d);
}

static void build_stream_query(struct dstr *q, const struct mc_config *c)
{
	dstr_printf(q, "/stream?w=%d&h=%d&fps=%d&bitrate=%d&audio=%d&camera=", c->width, c->height, c->fps,
		    c->bitrate_kbps, c->audio ? 1 : 0);
	url_encode(q, c->camera);
	if (c->control) {
		dstr_catf(q, "&zoom=%.2f&ev=%.2f&stab=%d", c->zoom, c->ev, c->stab ? 1 : 0);
		dstr_cat(q, "&wb=");
		url_encode(q, c->wb);
		dstr_cat(q, "&focus=");
		url_encode(q, c->focus);
		dstr_cat(q, "&antibanding=");
		url_encode(q, c->antibanding);
	}
}

static void wait_or_wake(struct mc_source *s, DWORD ms)
{
	if (!s->stop)
		WaitForSingleObject(s->wake, ms);
}

static void stream_session(struct mc_source *s, const struct mc_config *cfg, LONG gen)
{
	const char *host = cfg->host;
	if (cfg->usb) {
		if (!adb_prepare(s, cfg->adb_path, cfg->port))
			return;
		host = "127.0.0.1";
	} else if (!*host) {
		set_status(s, "Укажите IP телефона или выберите его из списка");
		return;
	}

	set_status(s, "Подключение к %s:%d…", host, cfg->port);
	SOCKET sock = tcp_connect(host, cfg->port, 3000);
	if (sock == INVALID_SOCKET) {
		set_status(s, "Нет связи с %s:%d — запущено ли приложение MagerCam?", host, cfg->port);
		return;
	}
	EnterCriticalSection(&s->lock);
	s->sock = sock;
	LeaveCriticalSection(&s->lock);

	set_recv_timeout(sock, 5000);
	struct dstr q = {0}, req = {0};
	build_stream_query(&q, cfg);
	dstr_printf(&req, "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MagerCam-OBS/" PLUGIN_VERSION "\r\n\r\n", q.array,
		    host);
	bool ok = send_all(sock, req.array, req.len);
	dstr_free(&q);
	dstr_free(&req);

	int code = ok ? read_http_header(sock) : -1;
	if (code == 409) {
		set_status(s, "Телефон занят: камера используется другим клиентом");
		ok = false;
	} else if (code != 200) {
		set_status(s, "Неверный ответ телефона (%d)", code);
		ok = false;
	}

	if (ok) {
		decoder_open(s, cfg->hw_decode);
		s->ts_valid = false;
		s->a_rate = 48000;
		s->a_ch = 1;
		EnterCriticalSection(&s->lock);
		s->connected = true;
		LeaveCriticalSection(&s->lock);
		set_status(s, "Подключено к %s", host);
	}

	while (ok && !s->stop && s->gen == gen) {
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
		size_t need = (size_t)len + AV_INPUT_BUFFER_PADDING_SIZE + 1;
		if (need > s->payload_cap) {
			bfree(s->payload);
			s->payload_cap = need * 2;
			s->payload = bmalloc(s->payload_cap);
		}
		if (len && !recv_all(sock, s->payload, len))
			break;
		memset(s->payload + len, 0, AV_INPUT_BUFFER_PADDING_SIZE + 1);

		EnterCriticalSection(&s->lock);
		int rot = s->rotation < 0 ? s->phone_orient : s->rotation;
		bool mirror = s->mirror;
		LeaveCriticalSection(&s->lock);

		switch (type) {
		case PKT_VIDEO_CONFIG:
			/* new encoder session: reset decoder to handle resolution change */
			decoder_open(s, cfg->hw_decode);
			decode_packet(s, s->payload, len, pts, true, rot, mirror);
			s->need_key = true;
			break;
		case PKT_VIDEO_FRAME:
			decode_packet(s, s->payload, len, pts, key, rot, mirror);
			break;
		case PKT_AUDIO_CONFIG:
			if (len >= 5) {
				s->a_rate = (int)(((uint32_t)s->payload[0] << 24) | ((uint32_t)s->payload[1] << 16) |
						  ((uint32_t)s->payload[2] << 8) | s->payload[3]);
				s->a_ch = s->payload[4];
			}
			break;
		case PKT_AUDIO: {
			if (!cfg->audio || s->a_ch < 1 || s->a_ch > 2)
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
	LeaveCriticalSection(&s->lock);
	closesocket(sock);
	decoder_close(s);

	if (was_connected) {
		if (!s->stop && s->gen == gen)
			set_status(s, "Соединение потеряно, переподключение…");
		if (clear && s->had_video)
			obs_source_output_video2(s->source, NULL);
		s->had_video = false;
	}
}

static DWORD WINAPI net_thread(LPVOID param)
{
	struct mc_source *s = param;
	os_set_thread_name("magercam-net");
	s->pkt = av_packet_alloc();
	s->frame = av_frame_alloc();
	s->sw_frame = av_frame_alloc();

	while (!s->stop) {
		EnterCriticalSection(&s->lock);
		struct mc_config cfg = s->cfg;
		bool idle = s->deactivate_hidden && !s->showing;
		LeaveCriticalSection(&s->lock);
		LONG gen = s->gen;

		if (idle) {
			set_status(s, "Неактивно (источник скрыт)");
			wait_or_wake(s, 1000);
			continue;
		}

		stream_session(s, &cfg, gen);
		if (s->stop)
			break;
		if (s->gen == gen)
			wait_or_wake(s, 1000);
	}

	av_packet_free(&s->pkt);
	av_frame_free(&s->frame);
	av_frame_free(&s->sw_frame);
	return 0;
}

static void force_reconnect(struct mc_source *s)
{
	InterlockedIncrement(&s->gen);
	EnterCriticalSection(&s->lock);
	if (s->sock != INVALID_SOCKET)
		shutdown(s->sock, SD_BOTH);
	LeaveCriticalSection(&s->lock);
	SetEvent(s->wake);
}

/* ------------------------------------------------------------------------- */
/* Control requests (/api/set), executed off the UI thread                   */

struct control_req {
	char host[128];
	int port;
	char path[512];
};

static DWORD WINAPI control_thread(LPVOID param)
{
	struct control_req *r = param;
	if (!http_get(r->host, r->port, r->path))
		LOG(LOG_WARNING, "control request failed: %s", r->path);
	bfree(r);
	return 0;
}

static void send_control(struct mc_source *s, const char *query)
{
	struct control_req *r = bzalloc(sizeof(*r));
	EnterCriticalSection(&s->lock);
	snprintf(r->host, sizeof(r->host), "%s", s->cfg.usb ? "127.0.0.1" : s->cfg.host);
	r->port = s->cfg.port;
	LeaveCriticalSection(&s->lock);
	snprintf(r->path, sizeof(r->path), "/api/set?%s", query);
	if (!*r->host) {
		bfree(r);
		return;
	}
	HANDLE h = CreateThread(NULL, 0, control_thread, r, 0, NULL);
	if (h)
		CloseHandle(h);
	else
		bfree(r);
}

/* ------------------------------------------------------------------------- */
/* OBS source callbacks                                                      */

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
	obs_data_set_default_bool(st, "audio", true);
	obs_data_set_default_int(st, "camera", 0);
	obs_data_set_default_string(st, "camera_id", "");
	obs_data_set_default_int(st, "rotation", -1);
	obs_data_set_default_bool(st, "mirror", false);
	obs_data_set_default_bool(st, "low_latency", true);
	obs_data_set_default_bool(st, "hw_decode", false);
	obs_data_set_default_bool(st, "deactivate_hidden", false);
	obs_data_set_default_bool(st, "clear_on_disconnect", false);
	obs_data_set_default_bool(st, "control", true);
	obs_data_set_default_double(st, "zoom", 1.0);
	obs_data_set_default_double(st, "ev", 0.0);
	obs_data_set_default_string(st, "wb", "auto");
	obs_data_set_default_string(st, "focus", "continuous");
	obs_data_set_default_string(st, "antibanding", "auto");
	obs_data_set_default_bool(st, "stab", false);
}

static void read_config(obs_data_t *st, struct mc_config *c)
{
	memset(c, 0, sizeof(*c));
	snprintf(c->host, sizeof(c->host), "%s", obs_data_get_string(st, "host"));
	/* trim spaces */
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
	c->audio = obs_data_get_bool(st, "audio");
	const char *cam = obs_data_get_string(st, "camera_id");
	if (!*cam) /* settings from older versions */
		cam = obs_data_get_int(st, "camera") == 1 ? "front" : "back";
	snprintf(c->camera, sizeof(c->camera), "%s", cam);
	c->hw_decode = obs_data_get_bool(st, "hw_decode");
	c->control = obs_data_get_bool(st, "control");
	c->zoom = obs_data_get_double(st, "zoom");
	c->ev = obs_data_get_double(st, "ev");
	snprintf(c->wb, sizeof(c->wb), "%s", obs_data_get_string(st, "wb"));
	snprintf(c->focus, sizeof(c->focus), "%s", obs_data_get_string(st, "focus"));
	snprintf(c->antibanding, sizeof(c->antibanding), "%s", obs_data_get_string(st, "antibanding"));
	c->stab = obs_data_get_bool(st, "stab");
}

static bool stream_params_differ(const struct mc_config *a, const struct mc_config *b)
{
	return strcmp(a->host, b->host) != 0 || a->port != b->port || a->usb != b->usb ||
	       strcmp(a->adb_path, b->adb_path) != 0 || a->width != b->width || a->height != b->height ||
	       a->fps != b->fps || a->audio != b->audio || strcmp(a->camera, b->camera) != 0 || a->hw_decode != b->hw_decode;
}

static void mc_update(void *data, obs_data_t *st)
{
	struct mc_source *s = data;
	struct mc_config c;
	read_config(st, &c);

	EnterCriticalSection(&s->lock);
	struct mc_config old = s->cfg;
	bool first = !s->initialized;
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

	SetEvent(s->wake); /* deactivate_hidden may have changed */

	if (!connected)
		return;

	struct dstr q = {0};
	if (old.bitrate_kbps != c.bitrate_kbps)
		dstr_catf(&q, "&bitrate=%d", c.bitrate_kbps);
	if (c.control) {
		if (old.zoom != c.zoom || !old.control)
			dstr_catf(&q, "&zoom=%.2f", c.zoom);
		if (old.ev != c.ev || !old.control)
			dstr_catf(&q, "&ev=%.2f", c.ev);
		if (strcmp(old.wb, c.wb) != 0 || !old.control) {
			dstr_cat(&q, "&wb=");
			url_encode(&q, c.wb);
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
	s->wake = CreateEventW(NULL, FALSE, FALSE, NULL);
	snprintf(s->status, sizeof(s->status), "Запуск…");
	mc_update(s, st);
	s->showing = obs_source_showing(source);
	s->thread = CreateThread(NULL, 0, net_thread, s, 0, NULL);
	return s;
}

static void mc_destroy(void *data)
{
	struct mc_source *s = data;
	s->stop = true;
	force_reconnect(s);
	if (s->thread) {
		WaitForSingleObject(s->thread, INFINITE);
		CloseHandle(s->thread);
	}
	CloseHandle(s->wake);
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
	SetEvent(s->wake);
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

/* --- properties --- */

static void fill_device_list(obs_property_t *list)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, "— выберите найденный телефон —", "");
	EnterCriticalSection(&g_found_lock);
	for (int i = 0; i < g_found_count; i++) {
		char label[200], value[96];
		snprintf(label, sizeof(label), "%s (%s)", g_found[i].name, g_found[i].ip);
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
	UNUSED_PARAMETER(p);
	const char *v = obs_data_get_string(st, "device");
	if (!v || !*v)
		return false;
	char ip[64];
	int port = DEFAULT_PORT;
	if (sscanf(v, "%63[^:]:%d", ip, &port) >= 1) {
		obs_data_set_string(st, "host", ip);
		obs_data_set_int(st, "port", port);
		obs_data_set_int(st, "conn", 0);
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

static bool on_discover(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	discovery_run(1500);
	fill_device_list(obs_properties_get(props, "device"));
	return true;
}

static bool on_reconnect(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	force_reconnect(data);
	return false;
}

static bool on_torch(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	send_control(data, "torch=toggle");
	return false;
}

static bool on_autofocus(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	send_control(data, "af=trigger&x=0.5&y=0.5");
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
	/* next lens reported by the phone; plain back/front toggle until it has reported them */
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

static bool on_refresh_status(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	struct mc_source *s = data;
	EnterCriticalSection(&s->lock);
	obs_property_set_description(obs_properties_get(props, "status"), s->status);
	LeaveCriticalSection(&s->lock);
	return true;
}

static obs_properties_t *mc_properties(void *data)
{
	struct mc_source *s = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	char status[300] = "Статус: —";
	if (s) {
		EnterCriticalSection(&s->lock);
		snprintf(status, sizeof(status), "Статус: %s", s->status);
		LeaveCriticalSection(&s->lock);
	}
	p = obs_properties_add_text(props, "status", status, OBS_TEXT_INFO);
	obs_properties_add_button(props, "refresh_status", "Обновить статус", on_refresh_status);

	/* connection */
	obs_properties_t *conn = obs_properties_create();
	p = obs_properties_add_list(conn, "conn", "Подключение", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Wi-Fi / локальная сеть", 0);
	obs_property_list_add_int(p, "USB (через ADB)", 1);
	obs_property_set_modified_callback(p, on_conn_changed);

	p = obs_properties_add_list(conn, "device", "Найденные телефоны", OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	if (g_found_count == 0)
		discovery_run(800);
	fill_device_list(p);
	obs_property_set_modified_callback2(p, on_device_selected, s);
	obs_properties_add_button(conn, "discover", "Найти телефоны в сети", on_discover);
	obs_properties_add_text(conn, "host", "IP телефона", OBS_TEXT_DEFAULT);
	obs_properties_add_int(conn, "port", "Порт", 1, 65535, 1);
	p = obs_properties_add_path(conn, "adb_path", "Путь к adb.exe (необязательно)", OBS_PATH_FILE, "adb.exe (adb.exe)",
				    NULL);
	obs_properties_add_button(conn, "reconnect", "Переподключиться", on_reconnect);
	obs_properties_add_group(props, "grp_conn", "Подключение", OBS_GROUP_NORMAL, conn);

	/* video */
	obs_properties_t *vid = obs_properties_create();
	p = obs_properties_add_list(vid, "resolution", "Разрешение", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "640x480 (4:3)", "640x480");
	obs_property_list_add_string(p, "1280x720 (HD)", "1280x720");
	obs_property_list_add_string(p, "1920x1080 (Full HD)", "1920x1080");
	obs_property_list_add_string(p, "2560x1440 (2K)", "2560x1440");
	obs_property_list_add_string(p, "3840x2160 (4K)", "3840x2160");
	p = obs_properties_add_list(vid, "fps", "Кадров в секунду", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "24", 24);
	obs_property_list_add_int(p, "25", 25);
	obs_property_list_add_int(p, "30", 30);
	obs_property_list_add_int(p, "50", 50);
	obs_property_list_add_int(p, "60", 60);
	p = obs_properties_add_int_slider(vid, "bitrate", "Битрейт (Мбит/с)", 1, 50, 1);
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
	p = obs_properties_add_list(vid, "rotation", "Поворот", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "Авто (по положению телефона)", -1);
	obs_property_list_add_int(p, "0°", 0);
	obs_property_list_add_int(p, "90°", 90);
	obs_property_list_add_int(p, "180°", 180);
	obs_property_list_add_int(p, "270°", 270);
	obs_properties_add_bool(vid, "mirror", "Отразить по горизонтали");
	obs_properties_add_bool(vid, "audio", "Передавать звук с микрофона телефона");
	obs_properties_add_group(props, "grp_video", "Видео и звук", OBS_GROUP_NORMAL, vid);

	/* camera controls */
	obs_properties_t *cam = obs_properties_create();
	obs_properties_add_bool(cam, "control", "Управлять камерой из OBS");
	obs_properties_add_button(cam, "torch", "Фонарик вкл/выкл", on_torch);
	obs_properties_add_button(cam, "autofocus", "Сфокусироваться (по центру)", on_autofocus);
	obs_properties_add_float_slider(cam, "zoom", "Зум", 1.0, 10.0, 0.1);
	obs_properties_add_float_slider(cam, "ev", "Экспозиция (EV)", -3.0, 3.0, 0.1);
	p = obs_properties_add_list(cam, "focus", "Фокус", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Непрерывный автофокус", "continuous");
	obs_property_list_add_string(p, "Автофокус по кнопке", "auto");
	obs_property_list_add_string(p, "Бесконечность", "infinity");
	obs_property_list_add_string(p, "Макро", "macro");
	p = obs_properties_add_list(cam, "wb", "Баланс белого", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Авто", "auto");
	obs_property_list_add_string(p, "Дневной свет", "daylight");
	obs_property_list_add_string(p, "Облачно", "cloudy");
	obs_property_list_add_string(p, "Тень", "shade");
	obs_property_list_add_string(p, "Лампа накаливания", "incandescent");
	obs_property_list_add_string(p, "Флуоресцентная лампа", "fluorescent");
	obs_property_list_add_string(p, "Сумерки", "twilight");
	p = obs_properties_add_list(cam, "antibanding", "Антимерцание", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "Авто", "auto");
	obs_property_list_add_string(p, "50 Гц (Россия/Европа)", "50");
	obs_property_list_add_string(p, "60 Гц", "60");
	obs_property_list_add_string(p, "Выкл", "off");
	obs_properties_add_bool(cam, "stab", "Стабилизация видео");
	obs_properties_add_group(props, "grp_cam", "Управление камерой", OBS_GROUP_NORMAL, cam);

	/* advanced */
	obs_properties_t *adv = obs_properties_create();
	obs_properties_add_bool(adv, "low_latency", "Минимальная задержка (без буферизации)");
	obs_properties_add_bool(adv, "hw_decode", "Аппаратное декодирование (D3D11)");
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
	WSADATA wsa;
	WSAStartup(MAKEWORD(2, 2), &wsa);
	InitializeCriticalSection(&g_found_lock);
	obs_register_source(&magercam_source);
	LOG(LOG_INFO, "plugin loaded, version " PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	DeleteCriticalSection(&g_found_lock);
	WSACleanup();
}

const char *obs_module_name(void)
{
	return "MagerCam";
}

const char *obs_module_description(void)
{
	return "Телефон как камера для OBS (MagerCam)";
}
