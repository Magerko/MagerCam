#pragma once
#include <stdbool.h>
#include <stdint.h>

struct decoder;

/* плоскости валидны только внутри вызова, держать указатели дольше нельзя */
typedef void (*decoder_frame_cb)(void *opaque, const uint8_t *y, const uint8_t *uv, int stride, int w, int h);

struct decoder *decoder_create(decoder_frame_cb cb, void *opaque);
void decoder_destroy(struct decoder *d);
/* false — декодер сломался, его надо пересоздать */
bool decoder_feed(struct decoder *d, const uint8_t *data, size_t len, int64_t pts_us);
