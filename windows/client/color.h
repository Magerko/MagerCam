/* Цветокоррекция на стороне ПК: яркость, контраст, насыщенность, теплота, LUT .cube и режим документа.
 * Все настройки сворачиваю в одну таблицу YUV -> YUV, поэтому на кадр уходит по одному чтению на пиксель. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../common/mcv_nv12.h"

struct color_params {
	int brightness, contrast, saturation, warmth; /* -100..100, 0 — без изменений */
	bool doc;                                      /* белая бумага и резкий текст */
};

struct color_lut; /* загруженный .cube */

struct color_lut *color_lut_load(const wchar_t *path, char *err, size_t errsz);
void color_lut_free(struct color_lut *l);

struct color_table;
/* NULL, если ничего менять не нужно */
struct color_table *color_table_build(const struct color_params *p, const struct color_lut *lut);
void color_table_free(struct color_table *t);
bool color_table_sharpen(const struct color_table *t);
/* src и dst одного размера; dst — свой буфер, src не трогаю */
void color_apply(const struct color_table *t, const struct nv12 *src, struct nv12 *dst);
