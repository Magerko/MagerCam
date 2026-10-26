/* H.264 (Annex B) -> NV12 with the Media Foundation decoder built into Windows. */
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct decoder;

/* Called for every decoded frame; planes are valid only during the call. */
typedef void (*decoder_frame_cb)(void *opaque, const uint8_t *y, const uint8_t *uv, int stride, int w, int h);

struct decoder *decoder_create(decoder_frame_cb cb, void *opaque);
void decoder_destroy(struct decoder *d);
/* Feeds one access unit (or SPS/PPS). Returns false on a fatal decoder error. */
bool decoder_feed(struct decoder *d, const uint8_t *data, size_t len, int64_t pts_us);
