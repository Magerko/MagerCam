/*
 * Окно «Настройки изображения» для источника MagerCam (только Windows).
 *
 * Окно обычное Win32 и живёт в потоке интерфейса OBS: цикл сообщений Qt разбирает сообщения всех окон своего
 * потока, так что отдельный поток с циклом не нужен. Значения пишу в настройки источника, а дальше их, как и раньше,
 * отправляет на телефон mc_update и сохраняет OBS вместе со сценой.
 *
 * Источник могут удалить, пока окно открыто. Поэтому окно трогает источник только через popup_ref(): под g_lock
 * беру сильную ссылку, и пока она у меня, mc_destroy не начнётся. Сам mc_destroy под тем же замком обнуляет p->s
 * и просит окно закрыться сообщением: уничтожать окно можно только в его потоке.
 */
#include "compat.h"
#include <commctrl.h>
#include <shlwapi.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <obs-module.h>
#include <util/bmem.h>
#include <util/platform.h>

#include "imgwin.h"

#define LOG(level, fmt, ...) blog(level, "[MagerCam] " fmt, ##__VA_ARGS__)

#define WM_MC_GONE (WM_APP + 1)
#define T_POLL 1
#define T_APPLY 2
#define APPLY_MS 100 /* ползунки шлю не чаще десяти раз в секунду */
#define IDLE_MS 2000 /* столько после правки руками не перетираю значения тем, что прислал телефон */
#define MANIFEST_ID 101

enum {
	ID_STATUS = 100,
	ID_CONTROL,
	ID_PROFILE,
	ID_ZOOM_L,
	ID_ZOOM,
	ID_EV_L,
	ID_EV,
	ID_FOCUS,
	ID_ANTIBAND,
	ID_SHARP,
	ID_NR,
	ID_PROC_HINT,
	ID_STAB,
	ID_LOCK,
	ID_TORCH,
	ID_AF,
	ID_REC,
	ID_WB,
	ID_TEMP_L,
	ID_TEMP,
	ID_TINT_L,
	ID_TINT,
	ID_WB_HINT,
	ID_FRAMING,
	ID_BG,
	ID_STR_L,
	ID_STR,
	ID_ROT,
	ID_MIRROR,
	ID_EXPO,
	ID_SAMT_L,
	ID_SAMT,
	ID_CLOSE,
};

enum { K_ZOOM = 1, K_EV = 2, K_TEMP = 4, K_TINT = 8, K_STR = 16, K_SAMT = 32 };

struct opt {
	const wchar_t *label;
	const char *value;
};

static const struct opt opt_profile[] = {{L"Свои настройки", ""},
					 {L"Звонок (720p, следить за лицом)", "call"},
					 {L"Стрим (1080p60)", "stream"},
					 {L"Документ", "doc"}};
static const struct opt opt_focus[] = {{L"Непрерывный автофокус", "continuous"},
				       {L"Автофокус по кнопке", "auto"},
				       {L"Бесконечность", "infinity"},
				       {L"Макро", "macro"}};
static const struct opt opt_ab[] = {{L"Авто", "auto"}, {L"50 Гц (Россия/Европа)", "50"}, {L"60 Гц", "60"}, {L"Выкл", "off"}};
static const struct opt opt_level[] = {{L"Выкл", "off"},
				       {L"Слабая", "low"},
				       {L"Стандарт (как у камеры)", "default"},
				       {L"Сильная", "high"}};
static const struct opt opt_wb[] = {{L"Авто", "auto"},
				    {L"Дневной свет", "daylight"},
				    {L"Облачно", "cloudy"},
				    {L"Тень", "shade"},
				    {L"Лампа накаливания", "incandescent"},
				    {L"Флуоресцентная лампа", "fluorescent"},
				    {L"Сумерки", "twilight"},
				    {L"Вручную (температура)", "manual"}};
static const struct opt opt_bg[] = {{L"Без обработки", "off"}, {L"Размыть", "blur"}, {L"Заменить картинкой", "image"}};
static const struct opt opt_rot[] = {
	{L"Авто (по положению телефона)", "-1"}, {L"0°", "0"}, {L"90°", "90"}, {L"180°", "180"}, {L"270°", "270"}};

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

struct combo_def {
	int id;
	const char *key;
	const struct opt *opts;
	int n;
	bool is_int;
};

static const struct combo_def combos[] = {
	{ID_PROFILE, "profile", opt_profile, N(opt_profile), false},
	{ID_FOCUS, "focus", opt_focus, N(opt_focus), false},
	{ID_ANTIBAND, "antibanding", opt_ab, N(opt_ab), false},
	{ID_SHARP, "sharp", opt_level, N(opt_level), false},
	{ID_NR, "nr", opt_level, N(opt_level), false},
	{ID_WB, "wb", opt_wb, N(opt_wb), false},
	{ID_BG, "bg", opt_bg, N(opt_bg), false},
	{ID_ROT, "rotation", opt_rot, N(opt_rot), true},
};

struct check_def {
	int id;
	const char *key;
};

static const struct check_def checks[] = {
	{ID_CONTROL, "control"}, {ID_STAB, "stab"}, {ID_LOCK, "lock"}, {ID_FRAMING, "framing"}, {ID_MIRROR, "mirror"},
};

#define MAX_CTL 80

struct popup {
	HWND hwnd;
	struct mc_source *s; /* под g_lock; NULL — источник удалён */
	struct popup *next;
	HFONT font;
	UINT dpi;
	struct {
		HWND wnd;
		short x, y, w, h;
	} ctl[MAX_CTL];
	int nctl;
	double zmin, zmax, emin, emax;
	bool ranges_set;
	unsigned dirty;
	ULONGLONG last_apply, last_user;
	/* пункты, которых камера не умеет: бит на пункт */
	unsigned sharp_off, nr_off;
	int sharp_sel, nr_sel;
	bool rec;
	bool dark; /* мало света: строку выдержки крашу как подсказку */
	bool proc_warn;
	wchar_t title[200];
};

static CRITICAL_SECTION g_lock;
static struct popup *g_list;
static HINSTANCE g_inst;
static ATOM g_atom;
static HANDLE g_actctx = INVALID_HANDLE_VALUE;
static HHOOK g_hook;


static HRESULT(WINAPI *p_GetDpiForMonitor)(HMONITOR, int, UINT *, UINT *);

/* ---------- источник ---------- */

static obs_source_t *popup_ref(struct popup *p, struct mc_source **out)
{
	obs_source_t *src = NULL;
	EnterCriticalSection(&g_lock);
	if (p->s) {
		src = obs_source_get_ref(mc_obs_source(p->s));
		if (src)
			*out = p->s;
	}
	LeaveCriticalSection(&g_lock);
	return src;
}

static void set_setting(struct popup *p, void (*fill)(struct popup *, obs_data_t *, void *), void *arg)
{
	struct mc_source *s;
	obs_source_t *src = popup_ref(p, &s);
	if (!src)
		return;
	/* в обновление кладу только изменённые ключи: остальное в настройках источника не трогаю */
	obs_data_t *d = obs_data_create();
	fill(p, d, arg);
	obs_source_update(src, d);
	obs_data_release(d);
	obs_source_release(src);
}

static void send_query(struct popup *p, const char *q)
{
	struct mc_source *s;
	obs_source_t *src = popup_ref(p, &s);
	if (!src)
		return;
	mc_send_control_query(s, q);
	obs_source_release(src);
}

/* ---------- значения контролов ---------- */

static HWND item(struct popup *p, int id)
{
	return GetDlgItem(p->hwnd, id);
}

static double zoom_from_pos(const struct popup *p, int pos)
{
	if (p->zmax <= p->zmin)
		return p->zmin;
	return p->zmin * pow(p->zmax / p->zmin, pos / 100.0);
}

static int pos_from_zoom(const struct popup *p, double z)
{
	if (p->zmax <= p->zmin || z <= 0)
		return 0;
	int pos = (int)lround(100.0 * log(z / p->zmin) / log(p->zmax / p->zmin));
	return pos < 0 ? 0 : pos > 100 ? 100 : pos;
}

static int tb_pos(struct popup *p, int id)
{
	return (int)SendMessageW(item(p, id), TBM_GETPOS, 0, 0);
}

static void tb_set(struct popup *p, int id, int pos)
{
	if (tb_pos(p, id) != pos)
		SendMessageW(item(p, id), TBM_SETPOS, TRUE, pos);
}

static double cur_zoom(struct popup *p)
{
	return round(zoom_from_pos(p, tb_pos(p, ID_ZOOM)) * 100.0) / 100.0;
}

static void set_text_if(HWND h, const wchar_t *t)
{
	wchar_t old[512];
	GetWindowTextW(h, old, 512);
	if (wcscmp(old, t) != 0)
		SetWindowTextW(h, t);
}

static void update_labels(struct popup *p)
{
	wchar_t b[96];
	_snwprintf(b, 96, L"Зум: %.1f×", cur_zoom(p));
	set_text_if(item(p, ID_ZOOM_L), b);
	_snwprintf(b, 96, L"Экспозиция: %+.1f EV", tb_pos(p, ID_EV) / 10.0);
	set_text_if(item(p, ID_EV_L), b);
	_snwprintf(b, 96, L"Температура: %d K", tb_pos(p, ID_TEMP) * 100);
	set_text_if(item(p, ID_TEMP_L), b);
	_snwprintf(b, 96, L"Оттенок: %+d", tb_pos(p, ID_TINT));
	set_text_if(item(p, ID_TINT_L), b);
	_snwprintf(b, 96, L"Сила размытия: %d%%", tb_pos(p, ID_STR));
	set_text_if(item(p, ID_STR_L), b);
	_snwprintf(b, 96, L"Доп. резкость: %+d", tb_pos(p, ID_SAMT));
	set_text_if(item(p, ID_SAMT_L), b);
}

static const struct combo_def *combo_by_id(int id)
{
	for (int i = 0; i < N(combos); i++)
		if (combos[i].id == id)
			return &combos[i];
	return NULL;
}

static const char *combo_value(struct popup *p, int id)
{
	const struct combo_def *c = combo_by_id(id);
	int i = (int)SendMessageW(item(p, id), CB_GETCURSEL, 0, 0);
	return c && i >= 0 && i < c->n ? c->opts[i].value : "";
}

static void combo_select(struct popup *p, int id, const char *v)
{
	const struct combo_def *c = combo_by_id(id);
	HWND h = item(p, id);
	for (int i = 0; c && i < c->n; i++) {
		if (strcmp(c->opts[i].value, v) == 0) {
			if ((int)SendMessageW(h, CB_GETCURSEL, 0, 0) != i)
				SendMessageW(h, CB_SETCURSEL, i, 0);
			if (id == ID_SHARP)
				p->sharp_sel = i;
			else if (id == ID_NR)
				p->nr_sel = i;
			return;
		}
	}
}

static void check_set(struct popup *p, int id, bool on)
{
	HWND h = item(p, id);
	if ((SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED) != on)
		SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
}

static void enable(struct popup *p, int id, bool on)
{
	HWND h = item(p, id);
	if (!IsWindowEnabled(h) != !on)
		EnableWindow(h, on);
}

static bool in_list(const char *list, const char *v)
{
	size_t n = strlen(v);
	for (const char *q = list; q && *q;) {
		const char *e = strchr(q, ',');
		size_t len = e ? (size_t)(e - q) : strlen(q);
		if (len == n && strncmp(q, v, n) == 0)
			return true;
		q = e ? e + 1 : NULL;
	}
	return false;
}

static unsigned unsupported_levels(const char *modes)
{
	unsigned off = 0;
	for (int i = 0; i < N(opt_level); i++)
		if (strcmp(opt_level[i].value, "default") != 0 && !in_list(modes, opt_level[i].value))
			off |= 1u << i;
	return off;
}

/* ---------- применение ---------- */

static void fill_sliders(struct popup *p, obs_data_t *d, void *arg)
{
	unsigned keys = *(unsigned *)arg;
	if (keys & K_ZOOM)
		obs_data_set_double(d, "zoom", cur_zoom(p));
	if (keys & K_EV)
		obs_data_set_double(d, "ev", tb_pos(p, ID_EV) / 10.0);
	if (keys & K_TEMP)
		obs_data_set_int(d, "wb_temp", tb_pos(p, ID_TEMP) * 100);
	if (keys & K_TINT)
		obs_data_set_int(d, "wb_tint", tb_pos(p, ID_TINT));
	if (keys & K_STR)
		obs_data_set_double(d, "bg_strength", tb_pos(p, ID_STR) / 100.0);
	if (keys & K_SAMT)
		obs_data_set_int(d, "sharp_amount", tb_pos(p, ID_SAMT));
}

static void apply_dirty(struct popup *p)
{
	KillTimer(p->hwnd, T_APPLY);
	unsigned keys = p->dirty;
	p->dirty = 0;
	if (!keys)
		return;
	set_setting(p, fill_sliders, &keys);
	p->last_apply = GetTickCount64();
}

static void fill_combo(struct popup *p, obs_data_t *d, void *arg)
{
	const struct combo_def *c = arg;
	const char *v = combo_value(p, c->id);
	if (c->is_int)
		obs_data_set_int(d, c->key, atoi(v));
	else
		obs_data_set_string(d, c->key, v);
}

static void fill_check(struct popup *p, obs_data_t *d, void *arg)
{
	const struct check_def *c = arg;
	obs_data_set_bool(d, c->key, SendMessageW(item(p, c->id), BM_GETCHECK, 0, 0) == BST_CHECKED);
}

static void slider_moved(struct popup *p, int id, bool end)
{
	unsigned key = id == ID_ZOOM	? K_ZOOM
		       : id == ID_EV	? K_EV
		       : id == ID_TEMP	? K_TEMP
		       : id == ID_TINT	? K_TINT
		       : id == ID_SAMT ? K_SAMT
					: K_STR;
	update_labels(p);
	p->dirty |= key;
	p->last_user = GetTickCount64();
	ULONGLONG since = p->last_user - p->last_apply;
	if (end || since >= APPLY_MS)
		apply_dirty(p);
	else
		SetTimer(p->hwnd, T_APPLY, (UINT)(APPLY_MS - since), NULL);
}

/* ---------- состояние телефона ---------- */

static void refresh(struct popup *p, bool initial)
{
	struct mc_source *s;
	obs_source_t *src = popup_ref(p, &s);
	if (!src)
		return;
	struct mc_phone_img ph;
	mc_get_phone_img(s, &ph);
	obs_data_t *st = obs_source_get_settings(src);

	wchar_t title[200], *wname = NULL;
	os_utf8_to_wcs_ptr(obs_source_get_name(src), 0, &wname);
	_snwprintf(title, 200, L"Настройки изображения — %ls", wname ? wname : L"");
	bfree(wname);
	if (wcscmp(title, p->title) != 0) {
		wcscpy(p->title, title);
		SetWindowTextW(p->hwnd, title);
	}

	bool live = ph.connected && ph.valid;
	bool control = obs_data_get_bool(st, "control");
	bool from_phone = live && control;

	double zmin = 1.0, zmax = 10.0, emin = -3.0, emax = 3.0;
	if (live) {
		zmin = ph.zoom_min > 0 ? ph.zoom_min : 1.0;
		zmax = ph.zoom_max > zmin ? ph.zoom_max : zmin;
		emin = ph.ev_min;
		emax = ph.ev_max;
	}
	bool ranges = !p->ranges_set || zmin != p->zmin || zmax != p->zmax || emin != p->emin || emax != p->emax;
	if (ranges) {
		p->zmin = zmin;
		p->zmax = zmax;
		p->emin = emin;
		p->emax = emax;
		p->ranges_set = true;
		SendMessageW(item(p, ID_EV), TBM_SETRANGEMIN, FALSE, (LPARAM)lround(emin * 10));
		SendMessageW(item(p, ID_EV), TBM_SETRANGEMAX, TRUE, (LPARAM)lround(emax * 10));
	}

	bool dropped = false;
	for (int i = 0; i < N(combos); i++)
		dropped |= SendMessageW(item(p, combos[i].id), CB_GETDROPPEDSTATE, 0, 0) != 0;
	bool idle = initial || (GetTickCount64() - p->last_user > IDLE_MS && !GetCapture() && !dropped && !p->dirty);

	if (idle || ranges) {
		double z = from_phone ? ph.zoom : obs_data_get_double(st, "zoom");
		double ev = from_phone ? ph.ev : obs_data_get_double(st, "ev");
		tb_set(p, ID_ZOOM, pos_from_zoom(p, z));
		tb_set(p, ID_EV, (int)lround(ev * 10));
	}
	if (idle) {
		bool wbt = from_phone && ph.has_wb_temp;
		tb_set(p, ID_TEMP, (int)lround((wbt ? ph.wb_temp : (int)obs_data_get_int(st, "wb_temp")) / 100.0));
		tb_set(p, ID_TINT, wbt ? ph.wb_tint : (int)obs_data_get_int(st, "wb_tint"));
		tb_set(p, ID_STR, (int)lround((from_phone ? ph.bg_strength : obs_data_get_double(st, "bg_strength")) * 100));
		tb_set(p, ID_SAMT, from_phone && ph.has_sharp_amount ? ph.sharp_amount : (int)obs_data_get_int(st, "sharp_amount"));
		combo_select(p, ID_PROFILE, from_phone ? ph.profile : obs_data_get_string(st, "profile"));
		combo_select(p, ID_FOCUS, from_phone ? ph.focus : obs_data_get_string(st, "focus"));
		combo_select(p, ID_ANTIBAND, from_phone ? ph.antibanding : obs_data_get_string(st, "antibanding"));
		combo_select(p, ID_SHARP, from_phone && ph.has_sharp ? ph.sharp : obs_data_get_string(st, "sharp"));
		combo_select(p, ID_NR, from_phone && ph.has_nr ? ph.nr : obs_data_get_string(st, "nr"));
		combo_select(p, ID_WB, from_phone ? ph.wb : obs_data_get_string(st, "wb"));
		combo_select(p, ID_BG, from_phone ? ph.bg : obs_data_get_string(st, "bg"));
		char rot[16];
		snprintf(rot, sizeof(rot), "%d", (int)obs_data_get_int(st, "rotation"));
		combo_select(p, ID_ROT, rot);
		check_set(p, ID_CONTROL, control);
		check_set(p, ID_STAB, from_phone ? ph.stab : obs_data_get_bool(st, "stab"));
		check_set(p, ID_LOCK, from_phone ? ph.lock : obs_data_get_bool(st, "lock"));
		check_set(p, ID_FRAMING, from_phone ? ph.framing : obs_data_get_bool(st, "framing"));
		check_set(p, ID_MIRROR, obs_data_get_bool(st, "mirror"));
	}
	update_labels(p);

	const char *ver = mc_phone_version();
	bool manual = strcmp(combo_value(p, ID_WB), "manual") == 0;
	bool wb_ok = !live || ph.has_wb_manual;
	bool proc_ok = !live || ph.has_sharp;
	enable(p, ID_PROFILE, control);
	enable(p, ID_ZOOM, control && zmax > zmin + 0.01);
	enable(p, ID_ZOOM_L, control && zmax > zmin + 0.01);
	enable(p, ID_EV, control && emax > emin);
	enable(p, ID_EV_L, control && emax > emin);
	enable(p, ID_FOCUS, control);
	enable(p, ID_ANTIBAND, control);
	enable(p, ID_SHARP, control && proc_ok);
	enable(p, ID_NR, control && proc_ok);
	bool samt_ok = !live || ph.has_sharp_amount;
	enable(p, ID_SAMT, control && samt_ok);
	enable(p, ID_SAMT_L, control && samt_ok);
	enable(p, ID_STAB, control);
	enable(p, ID_LOCK, control);
	enable(p, ID_WB, control);
	enable(p, ID_TEMP, control && manual && wb_ok);
	enable(p, ID_TEMP_L, control && manual && wb_ok);
	enable(p, ID_TINT, control && manual && wb_ok);
	enable(p, ID_TINT_L, control && manual && wb_ok);
	enable(p, ID_FRAMING, control && (ph.framing_available || !live));
	enable(p, ID_BG, control);
	bool blur = strcmp(combo_value(p, ID_BG), "off") != 0;
	enable(p, ID_STR, control && blur);
	enable(p, ID_STR_L, control && blur);
	enable(p, ID_TORCH, live && ph.has_flash);
	enable(p, ID_AF, live);
	enable(p, ID_REC, live);

	unsigned sharp_off = live && ph.has_sharp ? unsupported_levels(ph.sharp_modes) : 0;
	unsigned nr_off = live && ph.has_nr ? unsupported_levels(ph.nr_modes) : 0;
	if (sharp_off != p->sharp_off || nr_off != p->nr_off) {
		p->sharp_off = sharp_off;
		p->nr_off = nr_off;
		InvalidateRect(item(p, ID_SHARP), NULL, TRUE);
		InvalidateRect(item(p, ID_NR), NULL, TRUE);
	}

	wchar_t b[400];
	if (live && !ph.has_wb_manual)
		_snwprintf(b, 400, L"Ручной баланс не работает: обновите приложение MagerCam на телефоне до %hs", ver);
	else
		b[0] = 0;
	set_text_if(item(p, ID_WB_HINT), b);
	bool warn = true;
	if (live && !ph.has_sharp)
		_snwprintf(b, 400, L"Резкость и шумоподавление: обновите приложение MagerCam на телефоне до %hs", ver);
	else if (live && (sharp_off || nr_off))
		_snwprintf(b, 400, L"Серые пункты эта камера не поддерживает");
	else {
		_snwprintf(b, 400, L"Для мягкой картинки: камера — Выкл, ползунок по вкусу");
		warn = false;
	}
	if (warn != p->proc_warn) {
		p->proc_warn = warn;
		InvalidateRect(item(p, ID_PROC_HINT), NULL, TRUE);
	}
	set_text_if(item(p, ID_PROC_HINT), b);

	char expo[200], e[96];
	bool dark = false;
	if (ph.connected && mc_format_exposure(&ph, e, sizeof(e), &dark))
		snprintf(expo, sizeof(expo), "%s%s", e, dark ? " — " MC_DARK_HINT : "");
	else
		expo[0] = 0;
	wchar_t *wexpo = NULL;
	os_utf8_to_wcs_ptr(expo, 0, &wexpo);
	if (dark != p->dark) {
		p->dark = dark;
		InvalidateRect(item(p, ID_EXPO), NULL, TRUE);
	}
	set_text_if(item(p, ID_EXPO), wexpo ? wexpo : L"");
	bfree(wexpo);

	if (p->rec != ph.rec || initial) {
		p->rec = ph.rec;
		SetWindowTextW(item(p, ID_REC), ph.rec ? L"■ Стоп запись" : L"● Запись");
	}

	wchar_t *wph = NULL, *wapp = NULL;
	os_utf8_to_wcs_ptr(ph.name, 0, &wph);
	os_utf8_to_wcs_ptr(ph.app, 0, &wapp);
	if (!ph.connected)
		_snwprintf(b, 400, L"Телефон не подключён. Изменения сохраняются в источнике и уйдут на телефон при подключении.");
	else if (!ph.valid)
		_snwprintf(b, 400, L"Подключено, жду состояние телефона…");
	else if (wapp && *wapp)
		_snwprintf(b, 400, L"Телефон: %ls, MagerCam %ls", wph ? wph : L"", wapp);
	else
		_snwprintf(b, 400, L"Телефон: %ls. Приложение на нём старой версии — обновите до %hs", wph ? wph : L"", ver);
	bfree(wph);
	bfree(wapp);
	if (!control) {
		size_t o = wcslen(b);
		_snwprintf(b + o, 400 - o, L"\nУправление камерой из OBS выключено: настройки камеры не отправляются.");
	}
	set_text_if(item(p, ID_STATUS), b);

	obs_data_release(st);
	obs_source_release(src);
}

/* ---------- раскладка ---------- */

#define S(p, v) MulDiv((v), (int)(p)->dpi, 96)

static HWND add(struct popup *p, const wchar_t *cls, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id)
{
	HWND c = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(p, x), S(p, y), S(p, w), S(p, h), p->hwnd,
				 (HMENU)(INT_PTR)id, g_inst, NULL);
	if (!c)
		return NULL;
	SendMessageW(c, WM_SETFONT, (WPARAM)p->font, FALSE);
	if (p->nctl < MAX_CTL) {
		p->ctl[p->nctl].wnd = c;
		p->ctl[p->nctl].x = (short)x;
		p->ctl[p->nctl].y = (short)y;
		p->ctl[p->nctl].w = (short)w;
		p->ctl[p->nctl].h = (short)h;
		p->nctl++;
	}
	return c;
}

#define COL_W 372
#define LBL_W 116
#define CLIENT_W (12 + COL_W + 12 + COL_W + 12)
#define CLIENT_H 524

static void add_label(struct popup *p, const wchar_t *t, int x, int y, int id)
{
	add(p, L"STATIC", t, SS_LEFT | SS_NOPREFIX, x, y + 4, LBL_W - 4, 20, id);
}

static void add_combo(struct popup *p, int gx, int y, const wchar_t *label, int id, bool owner_draw)
{
	add_label(p, label, gx + 12, y, -1);
	const struct combo_def *c = combo_by_id(id);
	DWORD style = CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | (owner_draw ? CBS_OWNERDRAWFIXED | CBS_HASSTRINGS : 0);
	HWND h = add(p, WC_COMBOBOXW, L"", style, gx + 12 + LBL_W, y, COL_W - 24 - LBL_W, 240, id);
	for (int i = 0; h && c && i < c->n; i++)
		SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)c->opts[i].label);
}

static void add_slider(struct popup *p, int gx, int y, int lid, int id, int lo, int hi, int page)
{
	add(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, gx + 12, y + 4, LBL_W + 20, 20, lid);
	HWND h = add(p, TRACKBAR_CLASSW, L"", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP, gx + 8 + LBL_W + 24, y, COL_W - 20 - LBL_W - 24,
		     26, id);
	SendMessageW(h, TBM_SETRANGEMIN, FALSE, lo);
	SendMessageW(h, TBM_SETPAGESIZE, 0, page);
	SendMessageW(h, TBM_SETPOS, FALSE, lo > 0 ? lo : hi < 0 ? hi : 0);
	/* перерисовка только здесь: TBM_SETPOS с прежней позицией бегунок не пересчитывает, и при диапазоне от минуса
	 * ноль остался бы нарисован у левого края */
	SendMessageW(h, TBM_SETRANGEMAX, TRUE, hi);
}

static void build(struct popup *p)
{
	const int x0 = 12, x1 = 12 + COL_W + 12;
	add(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, 8, CLIENT_W - 24, 36, ID_STATUS);
	add(p, WC_BUTTONW, L"Управлять камерой телефона из OBS", BS_AUTOCHECKBOX | WS_TABSTOP, x0, 48, CLIENT_W - 24, 20,
	    ID_CONTROL);

	add(p, WC_BUTTONW, L"Камера", BS_GROUPBOX, x0, 74, COL_W, 398, -1);
	add_combo(p, x0, 96, L"Профиль:", ID_PROFILE, false);
	add_slider(p, x0, 128, ID_ZOOM_L, ID_ZOOM, 0, 100, 10);
	add_slider(p, x0, 160, ID_EV_L, ID_EV, -30, 30, 5);
	add_combo(p, x0, 192, L"Фокус:", ID_FOCUS, false);
	add_combo(p, x0, 224, L"Антимерцание:", ID_ANTIBAND, false);
	add_combo(p, x0, 256, L"Резкость камеры:", ID_SHARP, true);
	add_slider(p, x0, 288, ID_SAMT_L, ID_SAMT, -100, 100, 10);
	add_combo(p, x0, 320, L"Шумоподавление:", ID_NR, true);
	add(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0 + 12, 350, COL_W - 24, 30, ID_PROC_HINT);
	add(p, WC_BUTTONW, L"Стабилизация видео", BS_AUTOCHECKBOX | WS_TABSTOP, x0 + 12, 380, COL_W - 24, 20, ID_STAB);
	add(p, WC_BUTTONW, L"Зафиксировать экспозицию и баланс белого", BS_AUTOCHECKBOX | WS_TABSTOP, x0 + 12, 404, COL_W - 24,
	    20, ID_LOCK);
	const int bw = (COL_W - 24 - 16) / 3;
	add(p, WC_BUTTONW, L"Фонарик", BS_PUSHBUTTON | WS_TABSTOP, x0 + 12, 432, bw, 28, ID_TORCH);
	add(p, WC_BUTTONW, L"Фокус по центру", BS_PUSHBUTTON | WS_TABSTOP, x0 + 12 + bw + 8, 432, bw, 28, ID_AF);
	add(p, WC_BUTTONW, L"● Запись", BS_PUSHBUTTON | WS_TABSTOP, x0 + 12 + 2 * (bw + 8), 432, bw, 28, ID_REC);

	add(p, WC_BUTTONW, L"Баланс белого", BS_GROUPBOX, x1, 74, COL_W, 154, -1);
	add_combo(p, x1, 96, L"Режим:", ID_WB, false);
	add_slider(p, x1, 128, ID_TEMP_L, ID_TEMP, 20, 100, 5);
	add_slider(p, x1, 160, ID_TINT_L, ID_TINT, -50, 50, 5);
	add(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x1 + 12, 192, COL_W - 24, 30, ID_WB_HINT);

	add(p, WC_BUTTONW, L"Фон и кадрирование", BS_GROUPBOX, x1, 236, COL_W, 116, -1);
	add(p, WC_BUTTONW, L"Следить за лицом (автокадрирование)", BS_AUTOCHECKBOX | WS_TABSTOP, x1 + 12, 258, COL_W - 24, 20,
	    ID_FRAMING);
	add_combo(p, x1, 284, L"Фон:", ID_BG, false);
	add_slider(p, x1, 316, ID_STR_L, ID_STR, 0, 100, 10);

	add(p, WC_BUTTONW, L"Картинка в OBS", BS_GROUPBOX, x1, 360, COL_W, 112, -1);
	add_combo(p, x1, 382, L"Поворот:", ID_ROT, false);
	add(p, WC_BUTTONW, L"Отразить по горизонтали", BS_AUTOCHECKBOX | WS_TABSTOP, x1 + 12, 412, COL_W - 24, 20, ID_MIRROR);

	add(p, L"STATIC", L"", SS_LEFT | SS_NOPREFIX, x0, 482, CLIENT_W - 24 - 112, 32, ID_EXPO);
	add(p, WC_BUTTONW, L"Закрыть", BS_PUSHBUTTON | WS_TABSTOP, CLIENT_W - 12 - 100, 484, 100, 28, ID_CLOSE);
}

static void make_font(struct popup *p)
{
	NONCLIENTMETRICSW ncm = {sizeof(ncm)};
	if (!SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, p->dpi)) {
		SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
		ncm.lfMessageFont.lfHeight = MulDiv(ncm.lfMessageFont.lfHeight, (int)p->dpi, (int)GetDpiForSystem());
	}
	if (p->font)
		DeleteObject(p->font);
	p->font = CreateFontIndirectW(&ncm.lfMessageFont);
}

static int item_height(struct popup *p)
{
	HDC dc = GetDC(p->hwnd);
	HGDIOBJ old = SelectObject(dc, p->font);
	TEXTMETRICW tm;
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	ReleaseDC(p->hwnd, dc);
	return tm.tmHeight + S(p, 4);
}

static void layout(struct popup *p)
{
	int ih = item_height(p);
	for (int i = 0; i < p->nctl; i++) {
		SetWindowPos(p->ctl[i].wnd, NULL, S(p, p->ctl[i].x), S(p, p->ctl[i].y), S(p, p->ctl[i].w), S(p, p->ctl[i].h),
			     SWP_NOZORDER | SWP_NOACTIVATE);
		SendMessageW(p->ctl[i].wnd, WM_SETFONT, (WPARAM)p->font, TRUE);
	}
	for (int id = ID_SHARP; id <= ID_NR; id++) {
		SendMessageW(item(p, id), CB_SETITEMHEIGHT, (WPARAM)-1, ih);
		SendMessageW(item(p, id), CB_SETITEMHEIGHT, 0, ih);
	}
}

static void window_size(struct popup *p, int *w, int *h)
{
	RECT r = {0, 0, S(p, CLIENT_W), S(p, CLIENT_H)};
	DWORD style = (DWORD)GetWindowLongW(p->hwnd, GWL_STYLE), ex = (DWORD)GetWindowLongW(p->hwnd, GWL_EXSTYLE);
	AdjustWindowRectExForDpi(&r, style, FALSE, ex, p->dpi);
	*w = r.right - r.left;
	*h = r.bottom - r.top;
}

/* ---------- окно ---------- */

static void draw_level_item(struct popup *p, DRAWITEMSTRUCT *di)
{
	if (di->itemID == (UINT)-1)
		return;
	unsigned off = di->CtlID == ID_SHARP ? p->sharp_off : p->nr_off;
	bool unsupported = (off >> di->itemID) & 1;
	bool sel = (di->itemState & ODS_SELECTED) != 0;
	bool disabled = (di->itemState & ODS_DISABLED) != 0;
	HBRUSH bg = GetSysColorBrush(sel && !disabled ? COLOR_HIGHLIGHT : COLOR_WINDOW);
	FillRect(di->hDC, &di->rcItem, bg);
	SetBkMode(di->hDC, TRANSPARENT);
	SetTextColor(di->hDC, GetSysColor(disabled || unsupported ? COLOR_GRAYTEXT : sel ? COLOR_HIGHLIGHTTEXT : COLOR_WINDOWTEXT));
	HGDIOBJ old = SelectObject(di->hDC, p->font);
	wchar_t t[128];
	_snwprintf(t, 128, L"%ls%ls", opt_level[di->itemID < (UINT)N(opt_level) ? di->itemID : 0].label,
		   unsupported && !(di->itemState & ODS_COMBOBOXEDIT) ? L" — камера не умеет" : L"");
	RECT r = di->rcItem;
	r.left += S(p, 4);
	DrawTextW(di->hDC, t, -1, &r, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS);
	SelectObject(di->hDC, old);
	if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT))
		DrawFocusRect(di->hDC, &di->rcItem);
}

static void on_command(struct popup *p, int id, int code, HWND ctl)
{
	if (id == ID_CLOSE || id == IDCANCEL) {
		apply_dirty(p);
		DestroyWindow(p->hwnd);
		return;
	}
	if (id == ID_TORCH && code == BN_CLICKED) {
		send_query(p, "torch=toggle");
		return;
	}
	if (id == ID_AF && code == BN_CLICKED) {
		send_query(p, "af=trigger&x=0.5&y=0.5");
		return;
	}
	if (id == ID_REC && code == BN_CLICKED) {
		send_query(p, p->rec ? "rec=0" : "rec=1");
		return;
	}
	const struct combo_def *c = combo_by_id(id);
	if (c && code == CBN_SELCHANGE) {
		if (id == ID_SHARP || id == ID_NR) {
			/* обычный список не умеет серые пункты: выбор недоступного уровня просто откатываю */
			unsigned off = id == ID_SHARP ? p->sharp_off : p->nr_off;
			int *last = id == ID_SHARP ? &p->sharp_sel : &p->nr_sel;
			int i = (int)SendMessageW(ctl, CB_GETCURSEL, 0, 0);
			if (i >= 0 && ((off >> i) & 1)) {
				SendMessageW(ctl, CB_SETCURSEL, *last, 0);
				MessageBeep(MB_OK);
				return;
			}
			*last = i;
		}
		p->last_user = GetTickCount64();
		set_setting(p, fill_combo, (void *)c);
		refresh(p, false);
		return;
	}
	for (int i = 0; i < N(checks); i++) {
		if (checks[i].id == id && code == BN_CLICKED) {
			p->last_user = GetTickCount64();
			set_setting(p, fill_check, (void *)&checks[i]);
			refresh(p, false);
			return;
		}
	}
}

static void unlink_popup(struct popup *p)
{
	EnterCriticalSection(&g_lock);
	for (struct popup **pp = &g_list; *pp; pp = &(*pp)->next) {
		if (*pp == p) {
			*pp = p->next;
			break;
		}
	}
	bool empty = g_list == NULL;
	LeaveCriticalSection(&g_lock);
	if (empty && g_hook) {
		UnhookWindowsHookEx(g_hook);
		g_hook = NULL;
	}
}

static LRESULT CALLBACK wnd_proc(HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	struct popup *p = (struct popup *)GetWindowLongPtrW(w, GWLP_USERDATA);
	if (msg == WM_NCCREATE) {
		p = ((CREATESTRUCTW *)lp)->lpCreateParams;
		p->hwnd = w;
		SetWindowLongPtrW(w, GWLP_USERDATA, (LONG_PTR)p);
	}
	if (!p)
		return DefWindowProcW(w, msg, wp, lp);

	switch (msg) {
	case WM_COMMAND:
		on_command(p, LOWORD(wp), HIWORD(wp), (HWND)lp);
		return 0;
	case WM_HSCROLL: {
		int id = GetDlgCtrlID((HWND)lp);
		if (id == ID_ZOOM || id == ID_EV || id == ID_TEMP || id == ID_TINT || id == ID_STR || id == ID_SAMT) {
			int code = LOWORD(wp);
			slider_moved(p, id, code == TB_ENDTRACK || code == TB_THUMBPOSITION);
		}
		return 0;
	}
	case WM_TIMER:
		if (wp == T_APPLY)
			apply_dirty(p);
		else if (wp == T_POLL)
			refresh(p, false);
		return 0;
	case WM_MEASUREITEM: {
		MEASUREITEMSTRUCT *mi = (MEASUREITEMSTRUCT *)lp;
		if (mi->CtlType == ODT_COMBOBOX && p->font)
			mi->itemHeight = (UINT)item_height(p);
		return TRUE;
	}
	case WM_DRAWITEM: {
		DRAWITEMSTRUCT *di = (DRAWITEMSTRUCT *)lp;
		if (di->CtlType == ODT_COMBOBOX) {
			draw_level_item(p, di);
			return TRUE;
		}
		break;
	}
	case WM_CTLCOLORSTATIC:
	case WM_CTLCOLORBTN: {
		/* иначе подписи и галочки рисуются плашками своего цвета поверх фона окна */
		HDC dc = (HDC)wp;
		int id = GetDlgCtrlID((HWND)lp);
		if (msg == WM_CTLCOLORSTATIC && (id == ID_WB_HINT || (id == ID_PROC_HINT && p->proc_warn) || (id == ID_EXPO && p->dark)))
			SetTextColor(dc, RGB(176, 72, 0));
		SetBkColor(dc, GetSysColor(COLOR_BTNFACE));
		SetBkMode(dc, TRANSPARENT);
		return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
	}
	case WM_DPICHANGED: {
		p->dpi = HIWORD(wp);
		make_font(p);
		layout(p);
		const RECT *r = (const RECT *)lp;
		SetWindowPos(w, NULL, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
		return 0;
	}
	case WM_MC_GONE:
		DestroyWindow(w);
		return 0;
	case WM_CLOSE:
		apply_dirty(p);
		DestroyWindow(w);
		return 0;
	case WM_DESTROY:
		KillTimer(w, T_POLL);
		KillTimer(w, T_APPLY);
		return 0;
	case WM_NCDESTROY:
		unlink_popup(p);
		SetWindowLongPtrW(w, GWLP_USERDATA, 0);
		if (p->font)
			DeleteObject(p->font);
		bfree(p);
		return DefWindowProcW(w, msg, wp, lp);
	}
	return DefWindowProcW(w, msg, wp, lp);
}

/* Цикл сообщений тут чужой (Qt), и IsDialogMessage он не зовёт: без этого крючка в окне не работают Tab и Esc. */
static LRESULT CALLBACK msg_hook(int code, WPARAM wp, LPARAM lp)
{
	if (code == HC_ACTION && wp == PM_REMOVE) {
		MSG *m = (MSG *)lp;
		if (m->hwnd && m->message >= WM_KEYFIRST && m->message <= WM_KEYLAST) {
			HWND root = GetAncestor(m->hwnd, GA_ROOT);
			if (root && (ATOM)GetClassLongPtrW(root, GCW_ATOM) == g_atom && IsDialogMessageW(root, m)) {
				m->message = WM_NULL;
				m->wParam = 0;
				m->lParam = 0;
			}
		}
	}
	return CallNextHookEx(g_hook, code, wp, lp);
}

/* Темы Windows (Common Controls 6) у плагина не включены манифестом OBS, поэтому даю окну свой контекст активации
 * из манифеста в ресурсах DLL. comctl32 гружу тоже внутри него: так подхватывается шестая версия. */
static bool ensure_ready(void)
{
	if (g_atom)
		return true;
	wchar_t path[MAX_PATH * 2];
	GetModuleFileNameW(g_inst, path, MAX_PATH * 2);
	ACTCTXW ac = {sizeof(ac)};
	ac.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
	ac.lpSource = path;
	ac.hModule = g_inst;
	ac.lpResourceName = MAKEINTRESOURCEW(MANIFEST_ID);
	g_actctx = CreateActCtxW(&ac);
	if (g_actctx == INVALID_HANDLE_VALUE)
		LOG(LOG_WARNING, "image settings: no activation context (%lu), controls will look old", GetLastError());

	ULONG_PTR cookie = 0;
	bool act = g_actctx != INVALID_HANDLE_VALUE && ActivateActCtx(g_actctx, &cookie);
	HMODULE cc = LoadLibraryW(L"comctl32.dll");
	BOOL(WINAPI * icc)(const INITCOMMONCONTROLSEX *) = cc ? (void *)GetProcAddress(cc, "InitCommonControlsEx") : NULL;
	INITCOMMONCONTROLSEX ic = {sizeof(ic), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
	if (!icc || !icc(&ic))
		LOG(LOG_WARNING, "image settings: InitCommonControlsEx failed");
	DLLGETVERSIONPROC dgv = cc ? (DLLGETVERSIONPROC)(void *)GetProcAddress(cc, "DllGetVersion") : NULL;
	DLLVERSIONINFO vi = {sizeof(vi)};
	if (dgv && SUCCEEDED(dgv(&vi)))
		LOG(LOG_INFO, "image settings: comctl32 %lu.%lu", vi.dwMajorVersion, vi.dwMinorVersion);
	if (act)
		DeactivateActCtx(0, cookie);

	WNDCLASSEXW wc = {sizeof(wc)};
	wc.lpfnWndProc = wnd_proc;
	wc.hInstance = g_inst;
	wc.hCursor = LoadCursor(NULL, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	wc.lpszClassName = L"MagerCamImageSettings";
	g_atom = RegisterClassExW(&wc);
	if (!g_atom)
		LOG(LOG_WARNING, "image settings: RegisterClassEx failed (%lu)", GetLastError());
	return g_atom != 0;
}

static BOOL CALLBACK find_main(HWND w, LPARAM lp)
{
	wchar_t t[8];
	/* видимость не требую: OBS бывает свёрнут в трей */
	if (!GetWindow(w, GW_OWNER) && GetWindowTextW(w, t, 8) >= 4 && wcsncmp(t, L"OBS ", 4) == 0) {
		*(HWND *)lp = w;
		return FALSE;
	}
	return TRUE;
}

/* Владелец — главное окно OBS: тогда окно держится над OBS, сворачивается вместе с ним, не висит отдельной кнопкой
 * на панели задач и переживает закрытие свойств источника. API фронтенда я не линкую, поэтому ищу окно сам:
 * среди окон этого потока то, чей заголовок начинается с «OBS », иначе корневого владельца активного окна OBS. */
static HWND find_owner(void)
{
	HWND main = NULL;
	EnumThreadWindows(GetCurrentThreadId(), find_main, (LPARAM)&main);
	if (main)
		return main;
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg)
		GetWindowThreadProcessId(fg, &pid);
	return fg && pid == GetCurrentProcessId() ? GetAncestor(fg, GA_ROOTOWNER) : NULL;
}

/* справа от активного окна OBS (обычно это свойства источника), если там хватает места, иначе слева или по центру */
static void place(HWND anchor, int w, int h, int *x, int *y)
{
	RECT a;
	if (!anchor || !GetWindowRect(anchor, &a)) {
		POINT pt;
		GetCursorPos(&pt);
		a.left = a.right = pt.x;
		a.top = a.bottom = pt.y;
	}
	MONITORINFO mi = {sizeof(mi)};
	GetMonitorInfoW(MonitorFromRect(&a, MONITOR_DEFAULTTONEAREST), &mi);
	RECT wk = mi.rcWork;
	*x = a.right + 8;
	if (*x + w > wk.right)
		*x = a.left - 8 - w;
	if (*x < wk.left)
		*x = wk.left + (wk.right - wk.left - w) / 2;
	*y = a.top;
	if (*y + h > wk.bottom)
		*y = wk.bottom - h;
	if (*y < wk.top)
		*y = wk.top;
}

static UINT dpi_at(HWND anchor)
{
	if (!anchor)
		return GetDpiForSystem();
	if (!p_GetDpiForMonitor) {
		HMODULE sh = LoadLibraryW(L"shcore.dll");
		if (sh)
			p_GetDpiForMonitor = (void *)GetProcAddress(sh, "GetDpiForMonitor");
	}
	UINT dx = 0, dy = 0;
	if (p_GetDpiForMonitor &&
	    SUCCEEDED(p_GetDpiForMonitor(MonitorFromWindow(anchor, MONITOR_DEFAULTTONEAREST), 0, &dx, &dy)) && dx)
		return dx;
	return GetDpiForWindow(anchor);
}

static void open_on_ui(struct mc_source *s)
{
	obs_source_t *src = obs_source_get_ref(mc_obs_source(s));
	if (!src)
		return;
	HWND existing = NULL;
	EnterCriticalSection(&g_lock);
	for (struct popup *q = g_list; q; q = q->next)
		if (q->s == s)
			existing = q->hwnd;
	LeaveCriticalSection(&g_lock);
	if (existing) {
		if (IsIconic(existing))
			ShowWindow(existing, SW_RESTORE);
		SetForegroundWindow(existing);
		obs_source_release(src);
		return;
	}
	if (!ensure_ready()) {
		obs_source_release(src);
		return;
	}

	HWND owner = find_owner();
	HWND fg = GetForegroundWindow();
	DWORD pid = 0;
	if (fg)
		GetWindowThreadProcessId(fg, &pid);
	HWND anchor = fg && pid == GetCurrentProcessId() ? fg : owner;

	struct popup *p = bzalloc(sizeof(*p));
	p->s = s;
	p->dpi = dpi_at(anchor);
	p->sharp_sel = p->nr_sel = 2;
	make_font(p);
	/* без WS_CLIPCHILDREN: рамки групп прозрачные, фон под ними рисует само окно */
	DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	DWORD ex = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
	HWND w = CreateWindowExW(ex, L"MagerCamImageSettings", L"Настройки изображения", style, 0, 0, 100, 100, owner, NULL,
				 g_inst, p);
	if (!w) {
		LOG(LOG_WARNING, "image settings: CreateWindowEx failed (%lu)", GetLastError());
		DeleteObject(p->font);
		bfree(p);
		obs_source_release(src);
		return;
	}
	EnterCriticalSection(&g_lock);
	p->next = g_list;
	g_list = p;
	LeaveCriticalSection(&g_lock);
	if (!g_hook)
		g_hook = SetWindowsHookExW(WH_GETMESSAGE, msg_hook, NULL, GetCurrentThreadId());

	UINT real = GetDpiForWindow(w);
	if (real && real != p->dpi) {
		p->dpi = real;
		make_font(p);
	}
	ULONG_PTR cookie = 0;
	bool act = g_actctx != INVALID_HANDLE_VALUE && ActivateActCtx(g_actctx, &cookie);
	build(p);
	if (act)
		DeactivateActCtx(0, cookie);

	int ww, wh, x, y;
	window_size(p, &ww, &wh);
	place(anchor, ww, wh, &x, &y);
	SetWindowPos(w, NULL, x, y, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE);
	/* окно могло оказаться на мониторе с другим масштабом */
	real = GetDpiForWindow(w);
	if (real && real != p->dpi) {
		p->dpi = real;
		make_font(p);
		layout(p);
		window_size(p, &ww, &wh);
		SetWindowPos(w, NULL, 0, 0, ww, wh, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOMOVE);
	}
	layout(p);
	refresh(p, true);
	SetTimer(w, T_POLL, 400, NULL);
	/* не ShowWindow: если OBS запущен свёрнутым, первый ShowWindow в процессе берёт режим из STARTUPINFO и свернёт окно */
	SetWindowPos(w, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_SHOWWINDOW);
	SetForegroundWindow(w);
	obs_source_release(src);
}

struct open_req {
	struct mc_source *s;
	obs_weak_source_t *weak;
};

static void open_task(void *param)
{
	struct open_req *r = param;
	/* пока запрос шёл в поток интерфейса, источник могли удалить: тогда сильную ссылку уже не получить */
	obs_source_t *src = obs_weak_source_get_source(r->weak);
	if (src) {
		open_on_ui(r->s);
		obs_source_release(src);
	}
	obs_weak_source_release(r->weak);
	bfree(r);
}

void imgwin_open(struct mc_source *s)
{
	/* из свойств кнопка нажимается в потоке интерфейса, а через obs-websocket — в его собственном потоке */
	if (obs_in_task_thread(OBS_TASK_UI)) {
		open_on_ui(s);
		return;
	}
	struct open_req *r = bzalloc(sizeof(*r));
	r->s = s;
	r->weak = obs_source_get_weak_source(mc_obs_source(s));
	obs_queue_task(OBS_TASK_UI, open_task, r, false);
}

void imgwin_source_gone(struct mc_source *s)
{
	EnterCriticalSection(&g_lock);
	for (struct popup *p = g_list; p; p = p->next) {
		if (p->s == s) {
			p->s = NULL;
			PostMessageW(p->hwnd, WM_MC_GONE, 0, 0);
		}
	}
	LeaveCriticalSection(&g_lock);
}

void imgwin_init(void)
{
	InitializeCriticalSection(&g_lock);
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			   (LPCWSTR)(void *)imgwin_init, &g_inst);
}

void imgwin_free(void)
{
	/* после выгрузки DLL оконная процедура пропадёт, поэтому окна, оставшиеся в этом потоке, закрываю сейчас */
	for (;;) {
		HWND w = NULL;
		EnterCriticalSection(&g_lock);
		for (struct popup *p = g_list; p && !w; p = p->next)
			if (GetWindowThreadProcessId(p->hwnd, NULL) == GetCurrentThreadId())
				w = p->hwnd;
		LeaveCriticalSection(&g_lock);
		if (!w)
			break;
		DestroyWindow(w);
	}
	if (g_hook && !g_list) {
		UnhookWindowsHookEx(g_hook);
		g_hook = NULL;
	}
	if (g_atom && !g_list)
		UnregisterClassW(L"MagerCamImageSettings", g_inst);
	if (g_actctx != INVALID_HANDLE_VALUE)
		ReleaseActCtx(g_actctx);
	DeleteCriticalSection(&g_lock);
}
