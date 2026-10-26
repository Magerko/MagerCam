#include "net.h"
#include <windows.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

wchar_t *utf8_to_wide(const char *s)
{
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
	wchar_t *w = malloc(sizeof(wchar_t) * (n + 1));
	MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
	w[n] = 0;
	return w;
}

void wide_to_utf8(const wchar_t *w, char *out, size_t sz)
{
	if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)sz, NULL, NULL) && sz)
		out[0] = 0;
}

/* ------------------------------------------------------------------------- */
/* Sockets                                                                   */

SOCKET tcp_connect(const char *host, int port, int timeout_ms)
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

void set_recv_timeout(SOCKET sock, int ms)
{
	DWORD t = (DWORD)ms;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof(t));
}

bool recv_all(SOCKET sock, uint8_t *buf, size_t n)
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

bool send_all(SOCKET sock, const char *buf, size_t n)
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

int read_http_header(SOCKET sock)
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

bool http_get(const char *host, int port, const char *path)
{
	SOCKET sock = tcp_connect(host, port, 2000);
	if (sock == INVALID_SOCKET)
		return false;
	set_recv_timeout(sock, 2000);
	char req[2048];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n", path, host);
	bool ok = n > 0 && n < (int)sizeof(req) && send_all(sock, req, (size_t)n);
	int code = ok ? read_http_header(sock) : -1;
	closesocket(sock);
	return code == 200;
}

void url_encode(char *out, size_t outsz, const char *v)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t o = 0;
	for (const unsigned char *p = (const unsigned char *)v; *p && o + 4 < outsz; p++) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
		    *p == '_' || *p == '.') {
			out[o++] = (char)*p;
		} else {
			out[o++] = '%';
			out[o++] = hex[*p >> 4];
			out[o++] = hex[*p & 15];
		}
	}
	out[o] = 0;
}

/* ------------------------------------------------------------------------- */
/* Discovery                                                                 */

int discovery_run(int timeout_ms, struct found_device *found, int max)
{
	SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == INVALID_SOCKET)
		return 0;
	int one = 1;
	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char *)&one, sizeof(one));
	struct sockaddr_in local = {0};
	local.sin_family = AF_INET;
	bind(sock, (struct sockaddr *)&local, sizeof(local));

	static const char msg[] = "MAGERCAM_DISCOVER";
	struct sockaddr_in dst = {0};
	dst.sin_family = AF_INET;
	dst.sin_port = htons(MC_DISCOVERY_PORT);
	dst.sin_addr.s_addr = INADDR_BROADCAST;
	sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));

	/* directed broadcast on every adapter: 255.255.255.255 goes out of one NIC only */
	ULONG size = 32 * 1024;
	IP_ADAPTER_ADDRESSES *addrs = malloc(size);
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
				uint32_t mask = 0xFFFFFFFFu << (32 - prefix);
				dst.sin_addr.s_addr = htonl((ip & mask) | ~mask);
				sendto(sock, msg, sizeof(msg) - 1, 0, (struct sockaddr *)&dst, sizeof(dst));
			}
		}
	}
	free(addrs);

	int count = 0;
	ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeout_ms;
	for (;;) {
		ULONGLONG now = GetTickCount64();
		if (now >= deadline)
			break;
		int left = (int)(deadline - now);
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
		struct json *d = json_parse(buf);
		if (!d)
			continue;
		if (json_num(d, "magercam", 0) >= 1 && count < max) {
			char ip[64];
			inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
			bool dup = false;
			for (int i = 0; i < count; i++)
				if (strcmp(found[i].ip, ip) == 0)
					dup = true;
			if (!dup) {
				snprintf(found[count].name, sizeof(found[count].name), "%s", json_str(d, "name", "Телефон"));
				snprintf(found[count].ip, sizeof(found[count].ip), "%s", ip);
				found[count].port = (int)json_num(d, "port", MC_DEFAULT_PORT);
				count++;
			}
		}
		json_free(d);
	}
	closesocket(sock);
	return count;
}

/* ------------------------------------------------------------------------- */
/* ADB                                                                       */

static bool run_process(const char *exe, const char *args, char *out, size_t outsz, DWORD timeout_ms)
{
	char cmd[2048];
	snprintf(cmd, sizeof(cmd), "\"%s\" %s", exe, args);
	wchar_t *wcmd = utf8_to_wide(cmd);
	SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
	HANDLE rd = NULL, wr = NULL;
	if (!CreatePipe(&rd, &wr, &sa, 0)) {
		free(wcmd);
		return false;
	}
	SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
	STARTUPINFOW si = {0};
	si.cb = sizeof(si);
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.wShowWindow = SW_HIDE;
	si.hStdOutput = wr;
	si.hStdError = wr;
	PROCESS_INFORMATION pi = {0};
	BOOL ok = CreateProcessW(NULL, wcmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
	free(wcmd);
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

static bool file_exists_w(const wchar_t *p)
{
	DWORD a = GetFileAttributesW(p);
	return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool find_adb(const char *configured, char *out, size_t sz)
{
	if (configured && *configured) {
		wchar_t *w = utf8_to_wide(configured);
		bool ok = file_exists_w(w);
		free(w);
		if (ok) {
			snprintf(out, sz, "%s", configured);
			return true;
		}
	}
	wchar_t path[MAX_PATH * 2];
	/* bundled next to MagerCam.exe */
	DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
	if (n) {
		wchar_t *slash = wcsrchr(path, L'\\');
		if (slash) {
			wcscpy(slash + 1, L"adb\\adb.exe");
			if (file_exists_w(path)) {
				wide_to_utf8(path, out, sz);
				return true;
			}
		}
	}
	if (SearchPathW(NULL, L"adb.exe", NULL, MAX_PATH, path, NULL)) {
		wide_to_utf8(path, out, sz);
		return true;
	}
	wchar_t local[MAX_PATH];
	if (GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH)) {
		static const wchar_t *subdirs[] = {L"\\Android\\Sdk\\platform-tools\\adb.exe",
						   L"\\Microsoft\\WinGet\\Links\\adb.exe"};
		for (size_t i = 0; i < sizeof(subdirs) / sizeof(subdirs[0]); i++) {
			_snwprintf(path, MAX_PATH * 2, L"%ls%ls", local, subdirs[i]);
			if (file_exists_w(path)) {
				wide_to_utf8(path, out, sz);
				return true;
			}
		}
	}
	return false;
}

bool adb_prepare(const char *adb_cfg, int port, char *err, size_t errsz)
{
	char adb[1024];
	if (!find_adb(adb_cfg, adb, sizeof(adb))) {
		snprintf(err, errsz, "USB: adb.exe не найден");
		return false;
	}
	char out[4096];
	run_process(adb, "start-server", NULL, 0, 15000);
	if (!run_process(adb, "devices", out, sizeof(out), 5000)) {
		snprintf(err, errsz, "USB: не удалось запустить adb");
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
		snprintf(err, errsz, unauthorized ? "USB: разрешите отладку на телефоне (диалог RSA-ключа)"
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
		snprintf(err, errsz, "USB: adb forward не удался: %s", out);
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------------- */
/* JSON                                                                      */

struct jp {
	const char *p;
};

static void jp_ws(struct jp *s)
{
	while (*s->p == ' ' || *s->p == '\t' || *s->p == '\r' || *s->p == '\n')
		s->p++;
}

static void put_utf8(char **o, unsigned cp)
{
	char *d = *o;
	if (cp < 0x80)
		*d++ = (char)cp;
	else if (cp < 0x800) {
		*d++ = (char)(0xC0 | (cp >> 6));
		*d++ = (char)(0x80 | (cp & 0x3F));
	} else if (cp < 0x10000) {
		*d++ = (char)(0xE0 | (cp >> 12));
		*d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*d++ = (char)(0x80 | (cp & 0x3F));
	} else {
		*d++ = (char)(0xF0 | (cp >> 18));
		*d++ = (char)(0x80 | ((cp >> 12) & 0x3F));
		*d++ = (char)(0x80 | ((cp >> 6) & 0x3F));
		*d++ = (char)(0x80 | (cp & 0x3F));
	}
	*o = d;
}

static char *jp_string(struct jp *s)
{
	if (*s->p != '"')
		return NULL;
	s->p++;
	const char *start = s->p;
	size_t cap = 0;
	while (*s->p && *s->p != '"') {
		if (*s->p == '\\' && s->p[1])
			s->p++;
		s->p++;
		cap++;
	}
	if (*s->p != '"')
		return NULL;
	char *out = malloc(cap * 4 + 1), *o = out;
	for (const char *q = start; q < s->p; q++) {
		if (*q != '\\') {
			*o++ = *q;
			continue;
		}
		q++;
		switch (*q) {
		case 'n': *o++ = '\n'; break;
		case 't': *o++ = '\t'; break;
		case 'r': *o++ = '\r'; break;
		case 'b': *o++ = '\b'; break;
		case 'f': *o++ = '\f'; break;
		case 'u': {
			unsigned cp = 0;
			for (int i = 1; i <= 4 && q[i]; i++) {
				char c = q[i];
				cp = cp * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
			}
			q += 4;
			if (cp >= 0xD800 && cp < 0xDC00 && q[1] == '\\' && q[2] == 'u') {
				unsigned lo = 0;
				for (int i = 3; i <= 6; i++) {
					char c = q[i];
					lo = lo * 16 + (unsigned)(c >= 'a' ? c - 'a' + 10 : c >= 'A' ? c - 'A' + 10 : c - '0');
				}
				cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
				q += 6;
			}
			put_utf8(&o, cp);
			break;
		}
		default: *o++ = *q;
		}
	}
	*o = 0;
	s->p++;
	return out;
}

static struct json *jp_value(struct jp *s, int depth)
{
	if (depth > 32)
		return NULL;
	jp_ws(s);
	struct json *j = calloc(1, sizeof(*j));
	char c = *s->p;
	if (c == '{' || c == '[') {
		bool obj = c == '{';
		j->type = obj ? JSON_OBJ : JSON_ARR;
		s->p++;
		struct json **tail = &j->child;
		jp_ws(s);
		if (*s->p == (obj ? '}' : ']')) {
			s->p++;
			return j;
		}
		for (;;) {
			char *key = NULL;
			if (obj) {
				jp_ws(s);
				key = jp_string(s);
				jp_ws(s);
				if (!key || *s->p != ':') {
					free(key);
					json_free(j);
					return NULL;
				}
				s->p++;
			}
			struct json *v = jp_value(s, depth + 1);
			if (!v) {
				free(key);
				json_free(j);
				return NULL;
			}
			v->key = key;
			*tail = v;
			tail = &v->next;
			jp_ws(s);
			if (*s->p == ',') {
				s->p++;
				continue;
			}
			if (*s->p == (obj ? '}' : ']')) {
				s->p++;
				return j;
			}
			json_free(j);
			return NULL;
		}
	}
	if (c == '"') {
		j->type = JSON_STR;
		j->str = jp_string(s);
		if (!j->str) {
			free(j);
			return NULL;
		}
		return j;
	}
	if (!strncmp(s->p, "true", 4) || !strncmp(s->p, "false", 5)) {
		j->type = JSON_BOOL;
		j->num = s->p[0] == 't';
		s->p += s->p[0] == 't' ? 4 : 5;
		return j;
	}
	if (!strncmp(s->p, "null", 4)) {
		s->p += 4;
		return j;
	}
	char *end;
	j->num = strtod(s->p, &end);
	if (end == s->p) {
		free(j);
		return NULL;
	}
	j->type = JSON_NUM;
	s->p = end;
	return j;
}

struct json *json_parse(const char *text)
{
	struct jp s = {text};
	return jp_value(&s, 0);
}

void json_free(struct json *j)
{
	while (j) {
		struct json *n = j->next;
		json_free(j->child);
		free(j->key);
		free(j->str);
		free(j);
		j = n;
	}
}

const struct json *json_get(const struct json *obj, const char *key)
{
	if (!obj || obj->type != JSON_OBJ)
		return NULL;
	for (const struct json *c = obj->child; c; c = c->next)
		if (c->key && !strcmp(c->key, key))
			return c;
	return NULL;
}

const char *json_str(const struct json *obj, const char *key, const char *def)
{
	const struct json *v = json_get(obj, key);
	return v && v->type == JSON_STR ? v->str : def;
}

double json_num(const struct json *obj, const char *key, double def)
{
	const struct json *v = json_get(obj, key);
	return v && (v->type == JSON_NUM || v->type == JSON_BOOL) ? v->num : def;
}

bool json_bool(const struct json *obj, const char *key, bool def)
{
	const struct json *v = json_get(obj, key);
	return v && (v->type == JSON_BOOL || v->type == JSON_NUM) ? v->num != 0 : def;
}
