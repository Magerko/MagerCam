/*
 * Общие для клиента и виртуальной камеры функции NV12: один аффинный билинейный ресемплер,
 * который за один проход поворачивает, отражает и вписывает кадр в другой.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

struct nv12 {
	uint8_t *y, *uv;
	int ys, uvs; /* шаги строк */
	int w, h;
};

static inline void nv12_fill_black(struct nv12 *d)
{
	for (int r = 0; r < d->h; r++)
		memset(d->y + (size_t)r * d->ys, 16, d->w);
	for (int r = 0; r < d->h / 2; r++)
		memset(d->uv + (size_t)r * d->uvs, 128, d->w);
}

/* dst(X, Y) = src(ax + bx*X + cx*Y, ay + by*X + cy*Y) билинейно, для X в [x0,x1), Y в [y0,y1).
 * `ch` = 1 для Y и 2 для чередующихся UV. Координаты считаю по центрам пикселей своей плоскости. */
static inline void nv12_map_plane(const uint8_t *src, int ss, int sw, int sh, uint8_t *dst, int ds, int x0, int y0,
				  int x1, int y1, double ax, double bx, double cx, double ay, double by, double cy, int ch)
{
	const int64_t one = 1 << 16;
	const int64_t maxx = (int64_t)(sw - 1) << 16, maxy = (int64_t)(sh - 1) << 16;
	const int64_t dxx = (int64_t)llround(bx * one), dyx = (int64_t)llround(by * one);
	for (int Y = y0; Y < y1; Y++) {
		int64_t fx = (int64_t)llround((ax + bx * x0 + cx * Y) * one);
		int64_t fy = (int64_t)llround((ay + by * x0 + cy * Y) * one);
		uint8_t *o = dst + (size_t)Y * ds + (size_t)x0 * ch;
		for (int X = x0; X < x1; X++, fx += dxx, fy += dyx) {
			int64_t px = fx < 0 ? 0 : fx > maxx ? maxx : fx;
			int64_t py = fy < 0 ? 0 : fy > maxy ? maxy : fy;
			int ix = (int)(px >> 16), iy = (int)(py >> 16);
			int wx = (int)(px & 0xFFFF) >> 8, wy = (int)(py & 0xFFFF) >> 8; /* 0..255 */
			int ix2 = ix + 1 < sw ? ix + 1 : ix, iy2 = iy + 1 < sh ? iy + 1 : iy;
			const uint8_t *r0 = src + (size_t)iy * ss, *r1 = src + (size_t)iy2 * ss;
			for (int c = 0; c < ch; c++) {
				int a = r0[ix * ch + c], b = r0[ix2 * ch + c];
				int d = r1[ix * ch + c], e = r1[ix2 * ch + c];
				int top = a * 256 + (b - a) * wx;
				int bot = d * 256 + (e - d) * wx;
				*o++ = (uint8_t)((top * 256 + (bot - top) * wy + (1 << 15)) >> 16);
			}
		}
	}
}

/*
 * Поворачиваю `s` по часовой на `rot` градусов (0/90/180/270), при необходимости отражаю по
 * горизонтали и вписываю в `d` с сохранением пропорций: с полями (fill == false) или с обрезкой
 * (fill == true). Непокрытые области заливаю чёрным.
 */
static inline void nv12_transform(const struct nv12 *s, struct nv12 *d, int rot, bool mirror, bool fill)
{
	rot = ((rot % 360) + 360) % 360;
	bool swap = rot == 90 || rot == 270;
	double rw = swap ? s->h : s->w, rh = swap ? s->w : s->h; /* размер после поворота */
	double scale = fill ? fmax(d->w / rw, d->h / rh) : fmin(d->w / rw, d->h / rh);
	double ow = rw * scale, oh = rh * scale;
	double ox = (d->w - ow) / 2, oy = (d->h - oh) / 2;

	int x0 = (int)ceil(ox), y0 = (int)ceil(oy);
	int x1 = (int)floor(ox + ow), y1 = (int)floor(oy + oh);
	x0 &= ~1; y0 &= ~1; /* держу границы чётными, иначе съедет выравнивание хромы */
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > d->w) x1 = d->w;
	if (y1 > d->h) y1 = d->h;
	x1 &= ~1; y1 &= ~1;
	if (x0 > 0 || y0 > 0 || x1 < d->w || y1 < d->h)
		nv12_fill_black(d);
	if (x1 <= x0 || y1 <= y0)
		return;

	for (int plane = 0; plane < 2; plane++) {
		double k = plane ? 0.5 : 1.0; /* разрешение плоскости относительно яркости */
		double sw = s->w * k, sh = s->h * k;
		double prw = rw * k, prh = rh * k;
		/* пиксель X плоскости назначения -> координата в повёрнутой плоскости u = (X + .5 - ox*k) / scale - .5 */
		double ub = 1 / scale, ua = (0.5 - ox * k) / scale - 0.5;
		double vb = 1 / scale, va = (0.5 - oy * k) / scale - 0.5;
		/* u = ua + ub*X, v = va + vb*Y; отражение: u -> prw-1-u */
		double uA = ua, uB = ub;
		if (mirror) {
			uA = prw - 1 - ua;
			uB = -ub;
		}
		/* повёрнутые (u, v) -> исходные (sx, sy) */
		double ax, bx, cx, ay, by, cy;
		switch (rot) {
		case 90: /* sx = v, sy = sh-1-u */
			ax = va; bx = 0; cx = vb;
			ay = sh - 1 - uA; by = -uB; cy = 0;
			break;
		case 180: /* sx = sw-1-u, sy = sh-1-v */
			ax = sw - 1 - uA; bx = -uB; cx = 0;
			ay = sh - 1 - va; by = 0; cy = -vb;
			break;
		case 270: /* sx = sw-1-v, sy = u */
			ax = sw - 1 - va; bx = 0; cx = -vb;
			ay = uA; by = uB; cy = 0;
			break;
		default:
			ax = uA; bx = uB; cx = 0;
			ay = va; by = 0; cy = vb;
		}
		(void)prh;
		if (plane == 0)
			nv12_map_plane(s->y, s->ys, s->w, s->h, d->y, d->ys, x0, y0, x1, y1, ax, bx, cx, ay, by, cy, 1);
		else
			nv12_map_plane(s->uv, s->uvs, s->w / 2, s->h / 2, d->uv, d->uvs, x0 / 2, y0 / 2, x1 / 2, y1 / 2, ax,
				       bx, cx, ay, by, cy, 2);
	}
}
