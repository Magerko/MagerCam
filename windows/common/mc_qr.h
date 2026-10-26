/*
 * Маленький генератор QR-кодов: байтовый режим, уровень коррекции M, версии 1–10 (до 213 байт).
 * Мне этого хватает для ссылки сопряжения, поэтому таблицы держу только для этих версий.
 * Подключаю как header-only: в одном .c перед #include объявляю MC_QR_IMPLEMENTATION.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MC_QR_MAX_SIZE 57 /* версия 10: 17 + 4 * 10 */

struct mc_qr {
	int size;
	uint8_t m[MC_QR_MAX_SIZE][MC_QR_MAX_SIZE]; /* 1 — тёмный модуль */
};

bool mc_qr_encode(const char *text, struct mc_qr *qr);

#ifdef MC_QR_IMPLEMENTATION

/* уровень M: всего кодовых слов, слов коррекции на блок, блоков группы 1 и 2 */
static const struct { int total, ecc, b1, b2; } mcqr_v[11] = {
	{0, 0, 0, 0},
	{26, 10, 1, 0}, {44, 16, 1, 0}, {70, 26, 1, 0}, {100, 18, 2, 0}, {134, 24, 2, 0},
	{172, 16, 4, 0}, {196, 18, 4, 0}, {242, 22, 2, 2}, {292, 22, 3, 2}, {346, 26, 4, 1},
};

static const uint8_t mcqr_align[11][4] = {
	{0}, {0}, {6, 18}, {6, 22}, {6, 26}, {6, 30}, {6, 34}, {6, 22, 38}, {6, 24, 42}, {6, 26, 46}, {6, 28, 50},
};

static uint8_t mcqr_exp[512], mcqr_log[256];

static void mcqr_gf_init(void)
{
	if (mcqr_exp[0])
		return;
	int x = 1;
	for (int i = 0; i < 255; i++) {
		mcqr_exp[i] = (uint8_t)x;
		mcqr_log[x] = (uint8_t)i;
		x <<= 1;
		if (x & 0x100)
			x ^= 0x11D;
	}
	for (int i = 255; i < 512; i++)
		mcqr_exp[i] = mcqr_exp[i - 255];
}

static uint8_t mcqr_mul(uint8_t a, uint8_t b)
{
	return (a && b) ? mcqr_exp[mcqr_log[a] + mcqr_log[b]] : 0;
}

/* остаток от деления данных на порождающий многочлен Рида — Соломона степени n */
static void mcqr_rs(const uint8_t *data, int len, uint8_t *ecc, int n)
{
	uint8_t gen[32] = {1};
	for (int i = 0; i < n; i++) {
		for (int j = i + 1; j > 0; j--)
			gen[j] = gen[j - 1] ^ mcqr_mul(gen[j], mcqr_exp[i]);
		gen[0] = mcqr_mul(gen[0], mcqr_exp[i]);
	}
	/* gen[k] — коэффициент при x^k, старший gen[n] = 1 */
	memset(ecc, 0, (size_t)n);
	for (int i = 0; i < len; i++) {
		uint8_t f = data[i] ^ ecc[0];
		memmove(ecc, ecc + 1, (size_t)n - 1);
		ecc[n - 1] = 0;
		for (int j = 0; j < n; j++)
			ecc[j] ^= mcqr_mul(gen[n - 1 - j], f);
	}
}

struct mcqr_grid {
	int size;
	uint8_t *m;   /* модуль */
	uint8_t *fn;  /* 1 — служебный модуль, маска и данные его не трогают */
};

#define MQ(g, r, c) (g)->m[(r) * (g)->size + (c)]
#define FQ(g, r, c) (g)->fn[(r) * (g)->size + (c)]

static void mcqr_set(struct mcqr_grid *g, int r, int c, int dark)
{
	MQ(g, r, c) = (uint8_t)dark;
	FQ(g, r, c) = 1;
}

static void mcqr_finder(struct mcqr_grid *g, int r0, int c0)
{
	for (int dr = -1; dr <= 7; dr++)
		for (int dc = -1; dc <= 7; dc++) {
			int r = r0 + dr, c = c0 + dc;
			if (r < 0 || c < 0 || r >= g->size || c >= g->size)
				continue;
			int d = (dr >= 0 && dr <= 6 && (dc == 0 || dc == 6)) || (dc >= 0 && dc <= 6 && (dr == 0 || dr == 6)) ||
				(dr >= 2 && dr <= 4 && dc >= 2 && dc <= 4);
			mcqr_set(g, r, c, d);
		}
}

static void mcqr_format(struct mcqr_grid *g, int mask)
{
	/* уровень M кодируется битами 00 */
	int data = (0 << 3) | mask, rem = data;
	for (int i = 0; i < 10; i++)
		rem = (rem << 1) ^ ((rem >> 9) * 0x537);
	int bits = ((data << 10) | rem) ^ 0x5412;
	int n = g->size;
	/* первая копия у левого верхнего искателя, вторая разбита между двумя другими */
	for (int i = 0; i <= 5; i++)
		mcqr_set(g, i, 8, (bits >> i) & 1);
	mcqr_set(g, 7, 8, (bits >> 6) & 1);
	mcqr_set(g, 8, 8, (bits >> 7) & 1);
	mcqr_set(g, 8, 7, (bits >> 8) & 1);
	for (int i = 9; i < 15; i++)
		mcqr_set(g, 8, 14 - i, (bits >> i) & 1);
	for (int i = 0; i < 8; i++)
		mcqr_set(g, 8, n - 1 - i, (bits >> i) & 1);
	for (int i = 8; i < 15; i++)
		mcqr_set(g, n - 15 + i, 8, (bits >> i) & 1);
	mcqr_set(g, n - 8, 8, 1); /* тёмный модуль, есть всегда */
}

static void mcqr_version_info(struct mcqr_grid *g, int ver)
{
	if (ver < 7)
		return;
	int rem = ver;
	for (int i = 0; i < 12; i++)
		rem = (rem << 1) ^ ((rem >> 11) * 0x1F25);
	long bits = ((long)ver << 12) | rem;
	for (int i = 0; i < 18; i++) {
		int bit = (bits >> i) & 1, a = g->size - 11 + i % 3, b = i / 3;
		mcqr_set(g, a, b, bit);
		mcqr_set(g, b, a, bit);
	}
}

static void mcqr_function_patterns(struct mcqr_grid *g, int ver)
{
	int n = g->size;
	for (int i = 0; i < n; i++) {
		mcqr_set(g, 6, i, i % 2 == 0);
		mcqr_set(g, i, 6, i % 2 == 0);
	}
	mcqr_finder(g, 0, 0);
	mcqr_finder(g, 0, n - 7);
	mcqr_finder(g, n - 7, 0);
	int na = 0;
	while (na < 4 && mcqr_align[ver][na])
		na++;
	for (int i = 0; i < na; i++)
		for (int j = 0; j < na; j++) {
			if ((i == 0 && j == 0) || (i == 0 && j == na - 1) || (i == na - 1 && j == 0))
				continue;
			int cr = mcqr_align[ver][i], cc = mcqr_align[ver][j];
			for (int dr = -2; dr <= 2; dr++)
				for (int dc = -2; dc <= 2; dc++)
					mcqr_set(g, cr + dr, cc + dc, (dr == -2 || dr == 2 || dc == -2 || dc == 2 || (dr == 0 && dc == 0)));
		}
	mcqr_format(g, 0); /* резервирую место под формат, настоящие биты запишу после выбора маски */
	mcqr_version_info(g, ver);
}

static int mcqr_mask_bit(int mask, int r, int c)
{
	switch (mask) {
	case 0: return (r + c) % 2 == 0;
	case 1: return r % 2 == 0;
	case 2: return c % 3 == 0;
	case 3: return (r + c) % 3 == 0;
	case 4: return (r / 2 + c / 3) % 2 == 0;
	case 5: return (r * c) % 2 + (r * c) % 3 == 0;
	case 6: return ((r * c) % 2 + (r * c) % 3) % 2 == 0;
	default: return ((r + c) % 2 + (r * c) % 3) % 2 == 0;
	}
}

/* штраф по правилам стандарта: чем меньше, тем легче код читается сканером */
static long mcqr_penalty(const struct mcqr_grid *g)
{
	int n = g->size;
	long p = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int a = 0; a < n; a++) {
			int run = 1;
			for (int b = 1; b <= n; b++) {
				int same = b < n && (pass ? MQ(g, b, a) == MQ(g, b - 1, a) : MQ(g, a, b) == MQ(g, a, b - 1));
				if (same) {
					run++;
				} else {
					if (run >= 5)
						p += run - 2;
					run = 1;
				}
			}
		}
	for (int r = 0; r + 1 < n; r++)
		for (int c = 0; c + 1 < n; c++) {
			int v = MQ(g, r, c);
			if (v == MQ(g, r + 1, c) && v == MQ(g, r, c + 1) && v == MQ(g, r + 1, c + 1))
				p += 3;
		}
	static const uint8_t pat1[11] = {1, 0, 1, 1, 1, 0, 1, 0, 0, 0, 0}, pat2[11] = {0, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1};
	for (int a = 0; a < n; a++)
		for (int b = 0; b + 11 <= n; b++) {
			int h1 = 1, h2 = 1, v1 = 1, v2 = 1;
			for (int k = 0; k < 11; k++) {
				if (MQ(g, a, b + k) != pat1[k]) h1 = 0;
				if (MQ(g, a, b + k) != pat2[k]) h2 = 0;
				if (MQ(g, b + k, a) != pat1[k]) v1 = 0;
				if (MQ(g, b + k, a) != pat2[k]) v2 = 0;
			}
			p += 40 * (h1 + h2 + v1 + v2);
		}
	int dark = 0;
	for (int i = 0; i < n * n; i++)
		dark += g->m[i];
	int pct = dark * 100 / (n * n);
	p += (pct > 50 ? pct - 50 : 50 - pct) / 5 * 10;
	return p;
}

bool mc_qr_encode(const char *text, struct mc_qr *qr)
{
	mcqr_gf_init();
	int len = (int)strlen(text), ver = 0;
	for (int v = 1; v <= 10; v++) {
		int blocks = mcqr_v[v].b1 + mcqr_v[v].b2;
		int data_cw = mcqr_v[v].total - mcqr_v[v].ecc * blocks;
		int count_bits = v < 10 ? 8 : 16;
		if (4 + count_bits + 8 * len <= data_cw * 8) {
			ver = v;
			break;
		}
	}
	if (!ver)
		return false;
	int blocks = mcqr_v[ver].b1 + mcqr_v[ver].b2, eccn = mcqr_v[ver].ecc;
	int data_cw = mcqr_v[ver].total - eccn * blocks;

	/* битовый поток: режим 0100, длина, байты, терминатор, выравнивание и заполнители 0xEC/0x11 */
	uint8_t data[400] = {0};
	int bit = 0;
#define PUT(val, nb) do { for (int _i = (nb) - 1; _i >= 0; _i--, bit++) if (((val) >> _i) & 1) data[bit >> 3] |= (uint8_t)(0x80 >> (bit & 7)); } while (0)
	PUT(4, 4);
	PUT(len, ver < 10 ? 8 : 16);
	for (int i = 0; i < len; i++)
		PUT((uint8_t)text[i], 8);
	int cap = data_cw * 8;
	int term = cap - bit < 4 ? cap - bit : 4;
	bit += term;
	bit = (bit + 7) & ~7;
	for (int pad = 0; bit < cap; pad ^= 1)
		PUT(pad ? 0x11 : 0xEC, 8);
#undef PUT

	/* делю на блоки (короткие идут первыми), считаю коррекцию и перемежаю */
	int short_len = data_cw / blocks, nlong = data_cw % blocks;
	uint8_t ecc[8][32];
	int off[8], blen[8];
	for (int b = 0, o = 0; b < blocks; b++) {
		blen[b] = short_len + (b >= blocks - nlong ? 1 : 0);
		off[b] = o;
		mcqr_rs(data + o, blen[b], ecc[b], eccn);
		o += blen[b];
	}
	uint8_t all[400];
	int k = 0;
	for (int i = 0; i <= short_len; i++)
		for (int b = 0; b < blocks; b++)
			if (i < blen[b])
				all[k++] = data[off[b] + i];
	for (int i = 0; i < eccn; i++)
		for (int b = 0; b < blocks; b++)
			all[k++] = ecc[b][i];

	int n = 17 + 4 * ver;
	uint8_t m[MC_QR_MAX_SIZE * MC_QR_MAX_SIZE], fn[MC_QR_MAX_SIZE * MC_QR_MAX_SIZE];
	memset(m, 0, sizeof(m));
	memset(fn, 0, sizeof(fn));
	struct mcqr_grid g = {n, m, fn};
	mcqr_function_patterns(&g, ver);

	/* данные змейкой по парам столбцов снизу вверх и обратно, столбец 6 пропускаю */
	int idx = 0;
	for (int right = n - 1; right >= 1; right -= 2) {
		if (right == 6)
			right = 5;
		for (int vert = 0; vert < n; vert++)
			for (int j = 0; j < 2; j++) {
				int c = right - j;
				int up = ((right + 1) & 2) == 0;
				int r = up ? n - 1 - vert : vert;
				if (FQ(&g, r, c))
					continue;
				int b = idx < k * 8 ? (all[idx >> 3] >> (7 - (idx & 7))) & 1 : 0;
				MQ(&g, r, c) = (uint8_t)b;
				idx++;
			}
	}

	int best = 0;
	long best_p = -1;
	uint8_t base[MC_QR_MAX_SIZE * MC_QR_MAX_SIZE];
	memcpy(base, m, sizeof(base));
	for (int mask = 0; mask < 8; mask++) {
		memcpy(m, base, sizeof(base));
		for (int r = 0; r < n; r++)
			for (int c = 0; c < n; c++)
				if (!FQ(&g, r, c) && mcqr_mask_bit(mask, r, c))
					MQ(&g, r, c) ^= 1;
		mcqr_format(&g, mask);
		long p = mcqr_penalty(&g);
		if (best_p < 0 || p < best_p) {
			best_p = p;
			best = mask;
		}
	}
	memcpy(m, base, sizeof(base));
	for (int r = 0; r < n; r++)
		for (int c = 0; c < n; c++)
			if (!FQ(&g, r, c) && mcqr_mask_bit(best, r, c))
				MQ(&g, r, c) ^= 1;
	mcqr_format(&g, best);

	qr->size = n;
	for (int r = 0; r < n; r++)
		for (int c = 0; c < n; c++)
			qr->m[r][c] = MQ(&g, r, c);
	return true;
}

#undef MQ
#undef FQ
#endif
