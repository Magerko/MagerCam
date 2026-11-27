/*
 * Окно «Настройки изображения» (только Windows) и то, что оно берёт у источника.
 * Само окно в imgwin.c, состояние телефона и отправку команд даёт magercam.c.
 */
#pragma once
#include <stdbool.h>

/* что телефон прислал в последнем статусе; окно получает копию, снятую под замком источника */
struct mc_phone_img {
	bool connected;     /* поток идёт */
	bool valid;         /* после подключения уже пришёл статус */
	unsigned seq;       /* растёт с каждым статусом и при обрыве */
	char name[96];
	char app[24];       /* версия приложения; пусто — телефон старше 1.3.0 и её не шлёт */
	double zoom, zoom_min, zoom_max, ev, ev_min, ev_max, bg_strength;
	char wb[32], focus[32], antibanding[16], bg[8], profile[16];
	int wb_temp, wb_tint;
	bool has_wb_temp, has_wb_manual;
	bool has_flash, torch, framing_available, framing, stab, lock, rec;
	/* резкость и шумоподавление: has_* — телефон знает эти ключи, *_modes — что из off/low/default/high умеет камера */
	bool has_sharp, has_nr;
	char sharp[12], nr[12];
	char sharp_modes[48], nr_modes[48]; /* через запятую */
	bool has_sharp_amount;
	int sharp_amount; /* своя резкость поверх камеры, -100..100 */
	/* фактическая выдержка и чувствительность; has_exp — телефон их сообщает */
	bool has_exp;
	int exp_us, iso, iso_max, frame_us; /* frame_us — дольше кадра выдержку не поднять */
};

#include <stdio.h>

/* «Выдержка 1/60 · ISO 1600»; dark — выдержка упёрлась в длину кадра и чувствительность высокая, то есть мало света */
static inline bool mc_format_exposure(const struct mc_phone_img *ph, char *out, size_t sz, bool *dark)
{
	*dark = false;
	out[0] = 0;
	if (!ph->valid || !ph->has_exp || ph->exp_us <= 0)
		return false;
	char e[32];
	if (ph->exp_us < 1000000)
		snprintf(e, sizeof(e), "1/%d", (int)(1000000.0 / ph->exp_us + 0.5));
	else
		snprintf(e, sizeof(e), "%.1f с", ph->exp_us / 1000000.0);
	if (ph->iso > 0)
		snprintf(out, sz, "Выдержка %s · ISO %d", e, ph->iso);
	else
		snprintf(out, sz, "Выдержка %s", e);
	bool long_exp = ph->frame_us > 0 && ph->exp_us * 10 >= ph->frame_us * 9;
	bool high_iso = ph->iso >= 1600 || (ph->iso_max > 0 && ph->iso * 2 >= ph->iso_max);
	*dark = long_exp && high_iso;
	return true;
}

#define MC_DARK_HINT "Мало света: камера подняла чувствительность — добавьте свет или снизьте к/с"

#ifdef _WIN32
#include <obs-module.h>

struct mc_source;

/* magercam.c */
obs_source_t *mc_obs_source(struct mc_source *s);
void mc_get_phone_img(struct mc_source *s, struct mc_phone_img *out);
void mc_send_control_query(struct mc_source *s, const char *query);
/* до какой версии советовать обновить приложение на телефоне */
const char *mc_phone_version(void);

/* imgwin.c */
void imgwin_init(void);
void imgwin_free(void);
/* можно звать из любого потока: окно всё равно создаётся в потоке интерфейса OBS */
void imgwin_open(struct mc_source *s);
/* из mc_destroy: окно отвязывается от источника и закрывается в своём потоке */
void imgwin_source_gone(struct mc_source *s);
#endif
