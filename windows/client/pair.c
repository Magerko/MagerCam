#define _CRT_RAND_S
#include "net.h"
#include "pair.h"
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#define MC_QR_IMPLEMENTATION
#include "../common/mc_qr.h"

static struct {
	HWND wnd, owner;
	HFONT font;
	int dpi;
	UINT done_msg;
	SOCKET listener;
	HANDLE thread;
	volatile bool stop;
	char nonce[24];
	struct mc_qr qr;
	bool qr_ok;
	wchar_t status[200];
	CRITICAL_SECTION lock;
	bool lock_ready;
} g;

#define WM_PAIR_STATUS (WM_APP + 40)

/* сначала адаптеры со шлюзом: это обычно домашняя сеть, а не виртуалки и VPN */
static int local_ips(char out[][64], int max)
{
	ULONG sz = 16 * 1024;
	IP_ADAPTER_ADDRESSES *a = malloc(sz);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
	ULONG r = GetAdaptersAddresses(AF_INET, flags, NULL, a, &sz);
	if (r == ERROR_BUFFER_OVERFLOW) {
		free(a);
		a = malloc(sz);
		r = GetAdaptersAddresses(AF_INET, flags, NULL, a, &sz);
	}
	int n = 0;
	for (int pass = 0; pass < 2 && r == NO_ERROR; pass++) {
		for (IP_ADAPTER_ADDRESSES *p = a; p && n < max; p = p->Next) {
			if (p->OperStatus != IfOperStatusUp || p->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
				continue;
			if ((pass == 0) != (p->FirstGatewayAddress != NULL))
				continue;
			for (IP_ADAPTER_UNICAST_ADDRESS *u = p->FirstUnicastAddress; u && n < max; u = u->Next) {
				struct sockaddr_in *sin = (struct sockaddr_in *)u->Address.lpSockaddr;
				const uint8_t *b = (const uint8_t *)&sin->sin_addr;
				if (b[0] == 127 || (b[0] == 169 && b[1] == 254))
					continue;
				snprintf(out[n++], 64, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
			}
		}
	}
	free(a);
	return n;
}

static void set_status(const wchar_t *s)
{
	EnterCriticalSection(&g.lock);
	wcsncpy(g.status, s, 199);
	g.status[199] = 0;
	LeaveCriticalSection(&g.lock);
	if (g.wnd)
		PostMessageW(g.wnd, WM_PAIR_STATUS, 0, 0);
}

static void handle_client(SOCKET c, const struct sockaddr_in *peer)
{
	set_recv_timeout(c, 4000);
	char buf[4096];
	int len = 0;
	/* жду две строки: «MCPAIR1 <nonce>» и JSON */
	while (len < (int)sizeof(buf) - 1) {
		int r = recv(c, buf + len, (int)sizeof(buf) - 1 - len, 0);
		if (r <= 0)
			break;
		len += r;
		buf[len] = 0;
		char *nl = strchr(buf, '\n');
		if (nl && strchr(nl + 1, '\n'))
			break;
	}
	buf[len] = 0;
	char *nl = strchr(buf, '\n');
	char want[64];
	snprintf(want, sizeof(want), "MCPAIR1 %s", g.nonce);
	if (!nl || strncmp(buf, want, strlen(want)) != 0) {
		send_all(c, "BAD\n", 4);
		return;
	}
	struct json *j = json_parse(nl + 1);
	if (!j) {
		send_all(c, "BAD\n", 4);
		return;
	}
	struct pair_result *res = calloc(1, sizeof(*res));
	snprintf(res->name, sizeof(res->name), "%s", json_str(j, "name", "Телефон"));
	snprintf(res->key, sizeof(res->key), "%s", json_str(j, "key", ""));
	res->port = (int)json_num(j, "port", MC_DEFAULT_PORT);
	/* адрес беру тот, с которого телефон до меня достучался: он точно маршрутизируется */
	const uint8_t *b = (const uint8_t *)&peer->sin_addr;
	snprintf(res->ip, sizeof(res->ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
	json_free(j);
	send_all(c, "OK\n", 3);
	PostMessageW(g.owner, g.done_msg, 0, (LPARAM)res);
}

static DWORD WINAPI listen_main(void *arg)
{
	(void)arg;
	while (!g.stop) {
		fd_set rf;
		FD_ZERO(&rf);
		FD_SET(g.listener, &rf);
		struct timeval tv = {0, 300000};
		if (select(0, &rf, NULL, NULL, &tv) <= 0)
			continue;
		struct sockaddr_in peer;
		int pl = sizeof(peer);
		SOCKET c = accept(g.listener, (struct sockaddr *)&peer, &pl);
		if (c == INVALID_SOCKET)
			continue;
		handle_client(c, &peer);
		closesocket(c);
	}
	return 0;
}

static SOCKET open_listener(int *port_out)
{
	for (int port = PAIR_PORT_FIRST; port <= PAIR_PORT_LAST; port++) {
		SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			return s;
		BOOL excl = TRUE;
		setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
		struct sockaddr_in a = {0};
		a.sin_family = AF_INET;
		a.sin_port = htons((u_short)port);
		a.sin_addr.s_addr = htonl(INADDR_ANY);
		if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(s, 4) == 0) {
			*port_out = port;
			return s;
		}
		closesocket(s);
	}
	return INVALID_SOCKET;
}

#define PS(x) MulDiv((x), g.dpi, 96)

static void paint(HWND w)
{
	PAINTSTRUCT ps;
	HDC dc = BeginPaint(w, &ps);
	RECT rc;
	GetClientRect(w, &rc);
	FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
	int box = PS(300), x0 = (rc.right - box) / 2, y0 = PS(16);
	RECT white = {x0, y0, x0 + box, y0 + box};
	FillRect(dc, &white, (HBRUSH)GetStockObject(WHITE_BRUSH));
	if (g.qr_ok) {
		/* по 4 модуля светлого поля с каждой стороны, иначе камера хуже находит код */
		int n = g.qr.size, cell = box / (n + 8), off = (box - cell * n) / 2;
		HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
		for (int r = 0; r < n; r++)
			for (int c = 0; c < n; c++)
				if (g.qr.m[r][c]) {
					RECT m = {x0 + off + c * cell, y0 + off + r * cell, x0 + off + (c + 1) * cell,
						  y0 + off + (r + 1) * cell};
					FillRect(dc, &m, black);
				}
	}
	SetBkMode(dc, TRANSPARENT);
	HFONT old = SelectObject(dc, g.font);
	RECT t = {PS(16), y0 + box + PS(12), rc.right - PS(16), rc.bottom - PS(8)};
	wchar_t text[400];
	EnterCriticalSection(&g.lock);
	_snwprintf(text, 400,
		   L"На телефоне откройте MagerCam и нажмите кнопку QR, затем наведите камеру на этот код. "
		   L"Телефон и компьютер должны быть в одной сети.\n\n%ls",
		   g.status);
	LeaveCriticalSection(&g.lock);
	text[399] = 0;
	DrawTextW(dc, text, -1, &t, DT_WORDBREAK | DT_CENTER);
	SelectObject(dc, old);
	EndPaint(w, &ps);
}

static LRESULT CALLBACK pair_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_PAINT:
		paint(w);
		return 0;
	case WM_PAIR_STATUS:
		InvalidateRect(w, NULL, FALSE);
		return 0;
	case WM_CLOSE:
		pair_close();
		return 0;
	}
	return DefWindowProcW(w, msg, wp, lp);
}

void pair_close(void)
{
	g.stop = true;
	if (g.thread) {
		WaitForSingleObject(g.thread, 3000);
		CloseHandle(g.thread);
		g.thread = NULL;
	}
	if (g.listener != INVALID_SOCKET && g.listener) {
		closesocket(g.listener);
		g.listener = INVALID_SOCKET;
	}
	if (g.wnd) {
		HWND w = g.wnd;
		g.wnd = NULL;
		DestroyWindow(w);
	}
}

bool pair_show(HWND owner, HINSTANCE inst, HFONT font, int dpi, UINT done_msg)
{
	if (!g.lock_ready) {
		InitializeCriticalSection(&g.lock);
		g.lock_ready = true;
		WNDCLASSEXW wc = {sizeof(wc)};
		wc.lpfnWndProc = pair_proc;
		wc.hInstance = inst;
		wc.hCursor = LoadCursor(NULL, IDC_ARROW);
		wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
		wc.lpszClassName = L"MagerCamPair";
		wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
		RegisterClassExW(&wc);
	}
	if (g.wnd) {
		SetForegroundWindow(g.wnd);
		return true;
	}
	g.owner = owner;
	g.font = font;
	g.dpi = dpi;
	g.done_msg = done_msg;
	g.stop = false;
	int port = 0;
	g.listener = open_listener(&port);
	if (g.listener == INVALID_SOCKET)
		return false;

	unsigned int r1 = 0, r2 = 0;
	rand_s(&r1);
	rand_s(&r2);
	snprintf(g.nonce, sizeof(g.nonce), "%08x%08x", r1, r2);
	char ips[4][64], hosts[300] = "";
	int n = local_ips(ips, 4);
	for (int i = 0; i < n; i++) {
		if (i)
			strcat(hosts, ",");
		strcat(hosts, ips[i]);
	}
	char url[400];
	snprintf(url, sizeof(url), "magercam://pair?h=%s&p=%d&n=%s", hosts, port, g.nonce);
	g.qr_ok = mc_qr_encode(url, &g.qr);
	wchar_t st[200];
	if (n == 0)
		_snwprintf(st, 200, L"Не нашёл ни одного сетевого адреса компьютера.");
	else
		_snwprintf(st, 200, L"Жду телефон… (порт %d)", port);
	set_status(st);

	RECT r = {0, 0, MulDiv(332, dpi, 96), MulDiv(440, dpi, 96)};
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
	AdjustWindowRect(&r, style, FALSE);
	RECT o;
	GetWindowRect(owner, &o);
	int ww = r.right - r.left, wh = r.bottom - r.top;
	g.wnd = CreateWindowExW(WS_EX_DLGMODALFRAME, L"MagerCamPair", L"Сопряжение по QR-коду", style,
				o.left + ((o.right - o.left) - ww) / 2, o.top + ((o.bottom - o.top) - wh) / 2, ww, wh, owner,
				NULL, inst, NULL);
	ShowWindow(g.wnd, SW_SHOW);
	g.thread = CreateThread(NULL, 0, listen_main, NULL, 0, NULL);
	return true;
}
