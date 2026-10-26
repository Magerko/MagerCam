#include "color.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

struct color_lut {
	int n;
	float *rgb; /* n*n*n*3, красный меняется быстрее всего, как в самом .cube */
	float dmin[3], dmax[3];
};

/* цветность беру с шагом 4: таблица 256 x 64 x 64 занимает 3 МБ и строится за доли секунды */
#define CT_UV 64

struct color_table {
	uint8_t *map; /* [y][u/4][v/4] -> Y U V */
	bool sharpen;
};

struct color_lut *color_lut_load(const wchar_t *path, char *err, size_t errsz)
{
	FILE *f = _wfopen(path, L"rb");
	if (!f) {
		snprintf(err, errsz, "не удалось открыть файл");
		return NULL;
	}
	struct color_lut *l = calloc(1, sizeof(*l));
	l->dmax[0] = l->dmax[1] = l->dmax[2] = 1;
	char line[512];
	int count = 0, total = 0;
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '#' || *p == '\r' || *p == '\n' || !*p)
			continue;
		if (!strncmp(p, "LUT_3D_SIZE", 11)) {
			l->n = atoi(p + 11);
			if (l->n < 2 || l->n > 256) {
				snprintf(err, errsz, "неподдерживаемый размер LUT_3D_SIZE %d", l->n);
				goto fail;
			}
			total = l->n * l->n * l->n;
			l->rgb = malloc(sizeof(float) * 3 * (size_t)total);
			continue;
		}
		if (!strncmp(p, "LUT_1D_SIZE", 11)) {
			snprintf(err, errsz, "одномерные LUT не поддерживаются, нужен 3D");
			goto fail;
		}
		if (!strncmp(p, "DOMAIN_MIN", 10)) {
			sscanf(p + 10, "%f %f %f", &l->dmin[0], &l->dmin[1], &l->dmin[2]);
			continue;
		}
		if (!strncmp(p, "DOMAIN_MAX", 10)) {
			sscanf(p + 10, "%f %f %f", &l->dmax[0], &l->dmax[1], &l->dmax[2]);
			continue;
		}
		if ((*p >= '0' && *p <= '9') || *p == '-' || *p == '.') {
			float r, g, b;
			if (!l->rgb || count >= total || sscanf(p, "%f %f %f", &r, &g, &b) != 3)
				continue;
			l->rgb[count * 3] = r;
			l->rgb[count * 3 + 1] = g;
			l->rgb[count * 3 + 2] = b;
			count++;
		}
		/* TITLE и прочие ключевые слова пропускаю */
	}
	fclose(f);
	f = NULL;
	if (!l->rgb || count != total) {
		snprintf(err, errsz, "файл не похож на 3D LUT .cube");
		goto fail;
	}
	return l;
fail:
	if (f)
		fclose(f);
	color_lut_free(l);
	return NULL;
}

void color_lut_free(struct color_lut *l)
{
	if (!l)
		return;
	free(l->rgb);
	free(l);
}

static float clamp01(float v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

static void lut_apply(const struct color_lut *l, float c[3])
{
	int n = l->n;
	float f[3];
	int i0[3], i1[3];
	for (int k = 0; k < 3; k++) {
		float span = l->dmax[k] - l->dmin[k];
		float x = clamp01(span > 0 ? (c[k] - l->dmin[k]) / span : c[k]) * (float)(n - 1);
		i0[k] = (int)x;
		if (i0[k] > n - 2)
			i0[k] = n - 2;
		i1[k] = i0[k] + 1;
		f[k] = x - (float)i0[k];
	}
	float out[3] = {0, 0, 0};
	/* трилинейная интерполяция по восьми соседним узлам */
	for (int corner = 0; corner < 8; corner++) {
		int r = corner & 1 ? i1[0] : i0[0], g = corner & 2 ? i1[1] : i0[1], b = corner & 4 ? i1[2] : i0[2];
		float w = (corner & 1 ? f[0] : 1 - f[0]) * (corner & 2 ? f[1] : 1 - f[1]) * (corner & 4 ? f[2] : 1 - f[2]);
		const float *v = l->rgb + ((size_t)b * n * n + (size_t)g * n + r) * 3;
		out[0] += w * v[0];
		out[1] += w * v[1];
		out[2] += w * v[2];
	}
	c[0] = out[0];
	c[1] = out[1];
	c[2] = out[2];
}

static uint8_t to_u8(float v)
{
	int i = (int)lrintf(v);
	return (uint8_t)(i < 0 ? 0 : i > 255 ? 255 : i);
}

struct color_table *color_table_build(const struct color_params *p, const struct color_lut *lut)
{
	bool identity = !p->brightness && !p->contrast && !p->saturation && !p->warmth && !p->doc && !lut;
	if (identity)
		return NULL;
	struct color_table *t = calloc(1, sizeof(*t));
	t->map = malloc((size_t)256 * CT_UV * CT_UV * 3);
	if (!t->map) {
		free(t);
		return NULL;
	}
	t->sharpen = p->doc;
	float bright = p->brightness / 250.0f, contrast = 1 + p->contrast / 100.0f, sat = 1 + p->saturation / 100.0f;
	float warm = p->warmth / 400.0f;
	if (p->doc) {
		/* для документа поднимаю контраст и приглушаю цвет: бумага становится белой, а не серой */
		contrast *= 1.25f;
		sat *= 0.6f;
	}
	uint8_t *o = t->map;
	for (int y = 0; y < 256; y++) {
		for (int u = 0; u < CT_UV; u++) {
			for (int v = 0; v < CT_UV; v++, o += 3) {
				/* BT.709 в ограниченном диапазоне, как и у телефона */
				float Y = (y - 16) / 219.0f, U = (u * 4 + 1.5f - 128) / 224.0f, V = (v * 4 + 1.5f - 128) / 224.0f;
				float c[3] = {Y + 1.5748f * V, Y - 0.1873f * U - 0.4681f * V, Y + 1.8556f * U};
				for (int k = 0; k < 3; k++) {
					c[k] += bright;
					if (p->doc)
						c[k] = (c[k] - 0.06f) / 0.86f; /* уровни: подтягиваю белую точку */
					c[k] = (c[k] - 0.5f) * contrast + 0.5f;
				}
				float l = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
				for (int k = 0; k < 3; k++)
					c[k] = l + (c[k] - l) * sat;
				c[0] *= 1 + warm;
				c[2] *= 1 - warm;
				for (int k = 0; k < 3; k++)
					c[k] = clamp01(c[k]);
				if (lut)
					lut_apply(lut, c);
				float Yo = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
				o[0] = to_u8(16 + 219 * Yo);
				o[1] = to_u8(128 + 224 * (c[2] - Yo) / 1.8556f);
				o[2] = to_u8(128 + 224 * (c[0] - Yo) / 1.5748f);
			}
		}
	}
	return t;
}

void color_table_free(struct color_table *t)
{
	if (!t)
		return;
	free(t->map);
	free(t);
}

bool color_table_sharpen(const struct color_table *t)
{
	return t && t->sharpen;
}

void color_apply(const struct color_table *t, const struct nv12 *src, struct nv12 *dst)
{
	int w = src->w & ~1, h = src->h & ~1;
	for (int r = 0; r < h; r += 2) {
		const uint8_t *y0 = src->y + (size_t)r * src->ys, *y1 = y0 + src->ys;
		const uint8_t *uv = src->uv + (size_t)(r / 2) * src->uvs;
		uint8_t *d0 = dst->y + (size_t)r * dst->ys, *d1 = d0 + dst->ys;
		uint8_t *duv = dst->uv + (size_t)(r / 2) * dst->uvs;
		const uint8_t *ya = r > 0 ? y0 - src->ys : y0, *yb = r + 2 < h ? y1 + src->ys : y1;
		for (int c = 0; c < w; c += 2) {
			int ui = uv[c] >> 2, vi = uv[c + 1] >> 2;
			int yy[4] = {y0[c], y0[c + 1], y1[c], y1[c + 1]};
			if (t->sharpen) {
				/* нерезкое маскирование по кресту из соседей: текст на бумаге читается заметно лучше */
				int cl = c > 0 ? c - 1 : c, cr = c + 2 < w ? c + 2 : c + 1;
				int n0 = (ya[c] + y1[c] + y0[cl] + y0[c + 1]) >> 2;
				int n1 = (ya[c + 1] + y1[c + 1] + y0[c] + y0[cr]) >> 2;
				int n2 = (y0[c] + yb[c] + y1[cl] + y1[c + 1]) >> 2;
				int n3 = (y0[c + 1] + yb[c + 1] + y1[c] + y1[cr]) >> 2;
				int nb[4] = {n0, n1, n2, n3};
				for (int k = 0; k < 4; k++) {
					int s = yy[k] + (yy[k] - nb[k]);
					yy[k] = s < 0 ? 0 : s > 255 ? 255 : s;
				}
			}
			const uint8_t *m0 = t->map + (((size_t)yy[0] * CT_UV + ui) * CT_UV + vi) * 3;
			const uint8_t *m1 = t->map + (((size_t)yy[1] * CT_UV + ui) * CT_UV + vi) * 3;
			const uint8_t *m2 = t->map + (((size_t)yy[2] * CT_UV + ui) * CT_UV + vi) * 3;
			const uint8_t *m3 = t->map + (((size_t)yy[3] * CT_UV + ui) * CT_UV + vi) * 3;
			d0[c] = m0[0];
			d0[c + 1] = m1[0];
			d1[c] = m2[0];
			d1[c + 1] = m3[0];
			/* цветность у NV12 общая на квадрат 2x2, беру среднее от четырёх пикселей */
			duv[c] = (uint8_t)((m0[1] + m1[1] + m2[1] + m3[1] + 2) >> 2);
			duv[c + 1] = (uint8_t)((m0[2] + m1[2] + m2[2] + m3[2] + 2) >> 2);
		}
	}
}
