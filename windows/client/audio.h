#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wchar.h>

#define AUDIO_MAX_DEVICES 32

struct audio_device {
	wchar_t id[256];
	wchar_t name[128];
};
int audio_list_devices(struct audio_device *out, int max);

struct audio_out;
struct audio_out *audio_open(const wchar_t *id, int rate, int channels);
void audio_write(struct audio_out *a, const int16_t *pcm, int frames);
void audio_close(struct audio_out *a);
