#define COBJMACROS
#include "audio.h"
#include <windows.h>
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <stdlib.h>

#define BUFFER_HNS 1000000 /* 100 мс */
#define MAX_LATENCY_MS 150

struct audio_out {
	IAudioClient *client;
	IAudioRenderClient *render;
	UINT32 frames;
	int rate, channels;
};

int audio_list_devices(struct audio_device *out, int max)
{
	IMMDeviceEnumerator *en = NULL;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)))
		return 0;
	IMMDeviceCollection *col = NULL;
	int n = 0;
	if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eRender, DEVICE_STATE_ACTIVE, &col))) {
		UINT count = 0;
		IMMDeviceCollection_GetCount(col, &count);
		for (UINT i = 0; i < count && n < max; i++) {
			IMMDevice *dev = NULL;
			if (FAILED(IMMDeviceCollection_Item(col, i, &dev)))
				continue;
			LPWSTR id = NULL;
			IPropertyStore *ps = NULL;
			if (SUCCEEDED(IMMDevice_GetId(dev, &id)) && SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
				PROPVARIANT v;
				PropVariantInit(&v);
				if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
					wcsncpy(out[n].name, v.pwszVal, 127);
					out[n].name[127] = 0;
					wcsncpy(out[n].id, id, 255);
					out[n].id[255] = 0;
					n++;
				}
				PropVariantClear(&v);
			}
			if (ps)
				IPropertyStore_Release(ps);
			CoTaskMemFree(id);
			IMMDevice_Release(dev);
		}
		IMMDeviceCollection_Release(col);
	}
	IMMDeviceEnumerator_Release(en);
	return n;
}

struct audio_out *audio_open(const wchar_t *id, int rate, int channels)
{
	IMMDeviceEnumerator *en = NULL;
	IMMDevice *dev = NULL;
	struct audio_out *a = calloc(1, sizeof(*a));
	a->rate = rate;
	a->channels = channels;
	if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&en)))
		goto fail;
	if (FAILED(IMMDeviceEnumerator_GetDevice(en, id, &dev)))
		goto fail;
	if (FAILED(IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&a->client)))
		goto fail;
	WAVEFORMATEX wf = {0};
	wf.wFormatTag = WAVE_FORMAT_PCM;
	wf.nChannels = (WORD)channels;
	wf.nSamplesPerSec = (DWORD)rate;
	wf.wBitsPerSample = 16;
	wf.nBlockAlign = (WORD)(channels * 2);
	wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
	/* в shared-режиме устройство принимает только свой mix format, поэтому прошу
	 * движок самому переводить мои 16 бит моно (AUTOCONVERTPCM) */
	DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
	if (FAILED(IAudioClient_Initialize(a->client, AUDCLNT_SHAREMODE_SHARED, flags, BUFFER_HNS, 0, &wf, NULL)))
		goto fail;
	IAudioClient_GetBufferSize(a->client, &a->frames);
	if (FAILED(IAudioClient_GetService(a->client, &IID_IAudioRenderClient, (void **)&a->render)))
		goto fail;
	/* подкладываю немного тишины в начало, чтобы джиттер сети не давал провалов */
	UINT32 pre = (UINT32)rate * 40 / 1000;
	BYTE *buf;
	if (pre < a->frames && SUCCEEDED(IAudioRenderClient_GetBuffer(a->render, pre, &buf)))
		IAudioRenderClient_ReleaseBuffer(a->render, pre, AUDCLNT_BUFFERFLAGS_SILENT);
	IAudioClient_Start(a->client);
	IMMDevice_Release(dev);
	IMMDeviceEnumerator_Release(en);
	return a;
fail:
	if (dev)
		IMMDevice_Release(dev);
	if (en)
		IMMDeviceEnumerator_Release(en);
	audio_close(a);
	return NULL;
}

void audio_write(struct audio_out *a, const int16_t *pcm, int frames)
{
	if (!a || !a->render)
		return;
	UINT32 pad = 0;
	if (FAILED(IAudioClient_GetCurrentPadding(a->client, &pad)))
		return;
	if (pad > (UINT32)a->rate * MAX_LATENCY_MS / 1000)
		return; /* в очереди слишком много: выкидываю кусок, чтобы задержка не росла */
	UINT32 room = a->frames - pad;
	UINT32 n = (UINT32)frames < room ? (UINT32)frames : room;
	BYTE *buf;
	if (n && SUCCEEDED(IAudioRenderClient_GetBuffer(a->render, n, &buf))) {
		memcpy(buf, pcm, (size_t)n * a->channels * 2);
		IAudioRenderClient_ReleaseBuffer(a->render, n, 0);
	}
}

void audio_close(struct audio_out *a)
{
	if (!a)
		return;
	if (a->client)
		IAudioClient_Stop(a->client);
	if (a->render)
		IAudioRenderClient_Release(a->render);
	if (a->client)
		IAudioClient_Release(a->client);
	free(a);
}
