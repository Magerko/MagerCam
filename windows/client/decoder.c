#define COBJMACROS
#include "decoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>

/* этого CLSID нет ни в одной import-библиотеке mingw, объявляю его сам */
static const GUID CLSID_H264Decoder = {0x62ce7e72, 0x4c71, 0x4d20, {0xb1, 0x5d, 0x45, 0x28, 0x31, 0xa8, 0x7d, 0x9d}};
#include <stdlib.h>

/* 'HEVC' в виде FOURCC-подтипа; в старых заголовках mingw его тоже может не быть */
static const GUID MY_MFVideoFormat_HEVC = {0x43564548, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* HEVC-декодер в Windows не встроен: его ставит «Расширения для видео HEVC» из Store или драйвер видеокарты.
 * Ищу синхронный MFT, асинхронные аппаратные требуют другой модели работы. */
static IMFTransform *create_hevc_mft(void)
{
	MFT_REGISTER_TYPE_INFO in = {MFMediaType_Video, MY_MFVideoFormat_HEVC};
	IMFActivate **acts = NULL;
	UINT32 n = 0;
	IMFTransform *mft = NULL;
	if (FAILED(MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
			     &in, NULL, &acts, &n)))
		return NULL;
	for (UINT32 i = 0; i < n; i++) {
		if (!mft && FAILED(IMFActivate_ActivateObject(acts[i], &IID_IMFTransform, (void **)&mft)))
			mft = NULL;
		IMFActivate_Release(acts[i]);
	}
	CoTaskMemFree(acts);
	return mft;
}

bool decoder_hevc_available(void)
{
	IMFTransform *m = create_hevc_mft();
	if (!m)
		return false;
	IMFTransform_Release(m);
	return true;
}

struct decoder {
	IMFTransform *mft;
	decoder_frame_cb cb;
	void *opaque;
	IMFSample *out_sample; /* свой сэмпл на выход, если декодер не выдаёт их сам */
	DWORD out_size;
	bool provides_samples;
	UINT32 w, h, stride, vis_w, vis_h;
};

static UINT64 get_u64(IMFMediaType *t, REFGUID key)
{
	UINT64 v = 0;
	IMFMediaType_GetUINT64(t, key, &v);
	return v;
}

static HRESULT set_output_nv12(struct decoder *d)
{
	IMFMediaType *t = NULL;
	HRESULT hr = MF_E_INVALIDMEDIATYPE;
	for (DWORD i = 0; SUCCEEDED(IMFTransform_GetOutputAvailableType(d->mft, 0, i, &t)); i++) {
		GUID sub;
		if (SUCCEEDED(IMFMediaType_GetGUID(t, &MF_MT_SUBTYPE, &sub)) && IsEqualGUID(&sub, &MFVideoFormat_NV12)) {
			hr = IMFTransform_SetOutputType(d->mft, 0, t, 0);
			if (SUCCEEDED(hr)) {
				UINT64 fs = get_u64(t, &MF_MT_FRAME_SIZE);
				d->w = (UINT32)(fs >> 32);
				d->h = (UINT32)fs;
				UINT32 stride = 0;
				if (FAILED(IMFMediaType_GetUINT32(t, &MF_MT_DEFAULT_STRIDE, &stride)) || (INT32)stride <= 0)
					stride = d->w;
				d->stride = stride;
				d->vis_w = d->w;
				d->vis_h = d->h;
				MFVideoArea area;
				UINT32 sz = 0;
				if (SUCCEEDED(IMFMediaType_GetBlob(t, &MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8 *)&area,
								   sizeof(area), &sz)) &&
				    sz == sizeof(area) && area.Area.cx > 0 && area.Area.cy > 0) {
					d->vis_w = (UINT32)area.Area.cx;
					d->vis_h = (UINT32)area.Area.cy;
				}
			}
			IMFMediaType_Release(t);
			break;
		}
		IMFMediaType_Release(t);
	}
	if (SUCCEEDED(hr)) {
		MFT_OUTPUT_STREAM_INFO info = {0};
		IMFTransform_GetOutputStreamInfo(d->mft, 0, &info);
		d->provides_samples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
						       MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
		DWORD need = info.cbSize ? info.cbSize : d->stride * d->h * 3 / 2;
		if (!d->provides_samples && (!d->out_sample || need > d->out_size)) {
			if (d->out_sample)
				IMFSample_Release(d->out_sample);
			d->out_sample = NULL;
			IMFMediaBuffer *b = NULL;
			if (SUCCEEDED(MFCreateSample(&d->out_sample)) && SUCCEEDED(MFCreateMemoryBuffer(need, &b))) {
				IMFSample_AddBuffer(d->out_sample, b);
				d->out_size = need;
			}
			if (b)
				IMFMediaBuffer_Release(b);
		}
	}
	return hr;
}

struct decoder *decoder_create(decoder_frame_cb cb, void *opaque, bool hevc)
{
	struct decoder *d = calloc(1, sizeof(*d));
	d->cb = cb;
	d->opaque = opaque;
	HRESULT hr = S_OK;
	if (hevc) {
		d->mft = create_hevc_mft();
		if (!d->mft)
			hr = E_FAIL;
	} else {
		hr = CoCreateInstance(&CLSID_H264Decoder, NULL, CLSCTX_INPROC_SERVER, &IID_IMFTransform, (void **)&d->mft);
	}
	if (FAILED(hr)) {
		free(d);
		return NULL;
	}
	IMFAttributes *attrs = NULL;
	if (SUCCEEDED(IMFTransform_GetAttributes(d->mft, &attrs)) && attrs) {
		IMFAttributes_SetUINT32(attrs, &MF_LOW_LATENCY, TRUE);
		IMFAttributes_Release(attrs);
	}
	IMFMediaType *in = NULL;
	MFCreateMediaType(&in);
	IMFMediaType_SetGUID(in, &MF_MT_MAJOR_TYPE, &MFMediaType_Video);
	IMFMediaType_SetGUID(in, &MF_MT_SUBTYPE, hevc ? &MY_MFVideoFormat_HEVC : &MFVideoFormat_H264);
	IMFMediaType_SetUINT32(in, &MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
	hr = IMFTransform_SetInputType(d->mft, 0, in, 0);
	if (SUCCEEDED(hr) && FAILED(set_output_nv12(d))) {
		hr = E_FAIL;
		/* HEVC-декодеру без размера кадра нечего предложить на выход, а без выходного типа он не берёт вход.
		 * Даю ему примерный размер: настоящий он увидит в SPS и сообщит через STREAM_CHANGE. */
		if (hevc) {
			IMFMediaType_SetUINT64(in, &MF_MT_FRAME_SIZE, ((UINT64)1920 << 32) | 1080);
			if (SUCCEEDED(IMFTransform_SetInputType(d->mft, 0, in, 0)))
				hr = set_output_nv12(d);
		}
	}
	IMFMediaType_Release(in);
	if (FAILED(hr)) {
		decoder_destroy(d);
		return NULL;
	}
	/* эти сообщения необязательны, а «Расширения для видео HEVC» падают на них, пока выходной тип не задан */
	if (d->w) {
		IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
	}
	return d;
}

void decoder_destroy(struct decoder *d)
{
	if (!d)
		return;
	if (d->mft) {
		IMFTransform_ProcessMessage(d->mft, MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
		IMFTransform_Release(d->mft);
	}
	if (d->out_sample)
		IMFSample_Release(d->out_sample);
	free(d);
}

static void emit(struct decoder *d, IMFSample *s)
{
	IMFMediaBuffer *b = NULL;
	if (FAILED(IMFSample_ConvertToContiguousBuffer(s, &b)))
		return;
	BYTE *p = NULL;
	DWORD len = 0;
	if (SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, &len))) {
		size_t need = (size_t)d->stride * d->h * 3 / 2;
		if (len >= need && d->vis_w && d->vis_h)
			d->cb(d->opaque, p, p + (size_t)d->stride * d->h, (int)d->stride, (int)d->vis_w & ~1,
			      (int)d->vis_h & ~1);
		IMFMediaBuffer_Unlock(b);
	}
	IMFMediaBuffer_Release(b);
}

static bool drain(struct decoder *d)
{
	for (;;) {
		MFT_OUTPUT_DATA_BUFFER out = {0};
		out.dwStreamID = 0;
		if (!d->provides_samples && d->out_sample) {
			IMFMediaBuffer *b = NULL;
			if (SUCCEEDED(IMFSample_GetBufferByIndex(d->out_sample, 0, &b))) {
				IMFMediaBuffer_SetCurrentLength(b, 0);
				IMFMediaBuffer_Release(b);
			}
			out.pSample = d->out_sample;
		} else if (!d->provides_samples && d->w) {
			return false;
		}
		/* без выходного типа (d->w == 0) декодер ответит STREAM_CHANGE, как только разберёт SPS */
		DWORD status = 0;
		HRESULT hr = IMFTransform_ProcessOutput(d->mft, 0, 1, &out, &status);
		if (out.pEvents)
			IMFCollection_Release(out.pEvents);
		if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
			return true;
		if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
			if (FAILED(set_output_nv12(d)))
				return false;
			continue;
		}
		if (FAILED(hr))
			return false;
		if (out.pSample) {
			emit(d, out.pSample);
			if (d->provides_samples)
				IMFSample_Release(out.pSample);
		}
	}
}

bool decoder_feed(struct decoder *d, const uint8_t *data, size_t len, int64_t pts_us)
{
	IMFSample *s = NULL;
	IMFMediaBuffer *b = NULL;
	if (FAILED(MFCreateSample(&s)))
		return false;
	if (FAILED(MFCreateMemoryBuffer((DWORD)len, &b))) {
		IMFSample_Release(s);
		return false;
	}
	BYTE *p;
	IMFMediaBuffer_Lock(b, &p, NULL, NULL);
	memcpy(p, data, len);
	IMFMediaBuffer_Unlock(b);
	IMFMediaBuffer_SetCurrentLength(b, (DWORD)len);
	IMFSample_AddBuffer(s, b);
	IMFMediaBuffer_Release(b);
	IMFSample_SetSampleTime(s, pts_us * 10);

	HRESULT hr = IMFTransform_ProcessInput(d->mft, 0, s, 0);
	if (hr == MF_E_NOTACCEPTING) {
		drain(d);
		hr = IMFTransform_ProcessInput(d->mft, 0, s, 0);
	}
	IMFSample_Release(s);
	bool ok = drain(d);
	return SUCCEEDED(hr) || ok;
}
