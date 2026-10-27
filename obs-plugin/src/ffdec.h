/*
 * Декодер видео поверх FFmpeg, который уже есть в OBS.
 * В Windows ffdec.c собираю трижды, под заголовки FFmpeg 6, 7 и 8: раскладка AVFrame и AVCodecContext
 * между ними разная. При загрузке беру ту сборку, чья avcodec лежит рядом с obs64.exe, поэтому один
 * magercam.dll работает в OBS 30, 31, 32 и переживает обновление OBS. В Linux сборка одна, под системный FFmpeg.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum mc_pix { MC_PIX_OTHER, MC_PIX_I420, MC_PIX_NV12 };
enum mc_cs { MC_CS_UNKNOWN, MC_CS_709, MC_CS_601 };

struct mc_frame {
	uint8_t *data[3];
	int linesize[3];
	int width, height;
	enum mc_pix pix;
	int raw_format; /* номер AVPixelFormat, только для лога */
	bool full_range;
	enum mc_cs cs;
	int64_t pts_us;
};

enum mc_dec_result { MC_DEC_OK, MC_DEC_NEED_KEY, MC_DEC_HW_FAILED };

struct mc_dec;
typedef void (*mc_frame_fn)(void *opaque, const struct mc_frame *f);

struct mc_ffapi {
	const char *name; /* например "avcodec-61", для лога */
	bool (*has_hevc)(void);
	/* NULL, если декодера нет или он не открылся; *hw_failed ставлю, когда аппаратное декодирование недоступно */
	struct mc_dec *(*open)(bool hevc, bool hw, bool *hw_failed);
	void (*close)(struct mc_dec *d);
	enum mc_dec_result (*decode)(struct mc_dec *d, const uint8_t *data, size_t len, int64_t pts, bool key,
				     mc_frame_fn fn, void *opaque);
};

#ifdef _WIN32
const struct mc_ffapi *mc_ff6_api(bool load);
const struct mc_ffapi *mc_ff7_api(bool load);
const struct mc_ffapi *mc_ff8_api(bool load);
#else
const struct mc_ffapi *mc_ff_api(bool load);
#endif
