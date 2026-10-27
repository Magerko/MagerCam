/*
 * Декодирование H.264/H.265 через FFmpeg, см. ffdec.h.
 * В Windows файл собирается с -DMC_FFV=6/7/8 и заголовками нужной версии FFmpeg, функции беру через GetProcAddress
 * из avcodec/avutil, которые лежат рядом с obs64.exe. В Linux функции линкуются напрямую.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <util/base.h>
#include <util/bmem.h>

#include "ffdec.h"

#define LOG(level, fmt, ...) blog(level, "[MagerCam] " fmt, ##__VA_ARGS__)
#define STR2(x) #x
#define STR(x) STR2(x)
#define AVCODEC_NAME "avcodec-" STR(LIBAVCODEC_VERSION_MAJOR)
#define AVUTIL_NAME "avutil-" STR(LIBAVUTIL_VERSION_MAJOR)

#ifdef _WIN32
#define MC_HW_PIX AV_PIX_FMT_D3D11
#define MC_HW_DEVICE AV_HWDEVICE_TYPE_D3D11VA
#define MC_HW_NAME "D3D11VA"
#define CAT2(a, b) a##b
#define CAT(a, b) CAT2(a, b)
#define API_FN CAT(CAT(mc_ff, MC_FFV), _api)

/* Указатели на функции FFmpeg. Типы беру из самих заголовков, так что прототипы не разойдутся. */
#define AVCODEC_FUNCS(X)                  \
	X(avcodec_find_decoder)           \
	X(avcodec_alloc_context3)         \
	X(avcodec_open2)                  \
	X(avcodec_free_context)           \
	X(avcodec_send_packet)            \
	X(avcodec_receive_frame)          \
	X(avcodec_default_get_format)     \
	X(av_packet_alloc)                \
	X(av_packet_free)
#define AVUTIL_FUNCS(X)                   \
	X(av_frame_alloc)                 \
	X(av_frame_free)                  \
	X(av_frame_unref)                 \
	X(av_buffer_ref)                  \
	X(av_buffer_unref)                \
	X(av_hwdevice_ctx_create)         \
	X(av_hwframe_transfer_data)

#define DECLARE_FN(name) __typeof__(name) *p_##name;
static struct {
	AVCODEC_FUNCS(DECLARE_FN)
	AVUTIL_FUNCS(DECLARE_FN)
} ff;

#define avcodec_find_decoder ff.p_avcodec_find_decoder
#define avcodec_alloc_context3 ff.p_avcodec_alloc_context3
#define avcodec_open2 ff.p_avcodec_open2
#define avcodec_free_context ff.p_avcodec_free_context
#define avcodec_send_packet ff.p_avcodec_send_packet
#define avcodec_receive_frame ff.p_avcodec_receive_frame
#define avcodec_default_get_format ff.p_avcodec_default_get_format
#define av_packet_alloc ff.p_av_packet_alloc
#define av_packet_free ff.p_av_packet_free
#define av_frame_alloc ff.p_av_frame_alloc
#define av_frame_free ff.p_av_frame_free
#define av_frame_unref ff.p_av_frame_unref
#define av_buffer_ref ff.p_av_buffer_ref
#define av_buffer_unref ff.p_av_buffer_unref
#define av_hwdevice_ctx_create ff.p_av_hwdevice_ctx_create
#define av_hwframe_transfer_data ff.p_av_hwframe_transfer_data
#else
#define MC_HW_PIX AV_PIX_FMT_VAAPI
#define MC_HW_DEVICE AV_HWDEVICE_TYPE_VAAPI
#define MC_HW_NAME "VA-API"
#define API_FN mc_ff_api
#endif

struct mc_dec {
	AVCodecContext *ctx;
	AVBufferRef *hw_dev;
	AVPacket *pkt;
	AVFrame *frame;
	AVFrame *sw_frame;
};

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++)
		if (*p == MC_HW_PIX)
			return *p;
	return avcodec_default_get_format(ctx, fmts);
}

static void dec_close(struct mc_dec *d)
{
	if (!d)
		return;
	avcodec_free_context(&d->ctx);
	av_buffer_unref(&d->hw_dev);
	av_packet_free(&d->pkt);
	av_frame_free(&d->frame);
	av_frame_free(&d->sw_frame);
	bfree(d);
}

static struct mc_dec *dec_open(bool hevc, bool hw, bool *hw_failed)
{
	const AVCodec *codec = avcodec_find_decoder(hevc ? AV_CODEC_ID_HEVC : AV_CODEC_ID_H264);
	if (!codec)
		return NULL;
	struct mc_dec *d = bzalloc(sizeof(*d));
	d->ctx = avcodec_alloc_context3(codec);
	d->pkt = av_packet_alloc();
	d->frame = av_frame_alloc();
	d->sw_frame = av_frame_alloc();
	if (!d->ctx || !d->pkt || !d->frame || !d->sw_frame) {
		dec_close(d);
		return NULL;
	}
	d->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
	d->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
	d->ctx->thread_type = FF_THREAD_SLICE;
	d->ctx->thread_count = 0;

	if (hw && !*hw_failed) {
		if (av_hwdevice_ctx_create(&d->hw_dev, MC_HW_DEVICE, NULL, NULL, 0) == 0) {
			d->ctx->hw_device_ctx = av_buffer_ref(d->hw_dev);
			d->ctx->get_format = get_hw_format;
			LOG(LOG_INFO, "hardware decoding (" MC_HW_NAME ") enabled");
		} else {
			LOG(LOG_WARNING, MC_HW_NAME " unavailable, using software decoding");
			*hw_failed = true;
		}
	}

	if (avcodec_open2(d->ctx, codec, NULL) < 0) {
		dec_close(d);
		return NULL;
	}
	return d;
}

static void emit(AVFrame *f, mc_frame_fn fn, void *opaque)
{
	struct mc_frame out = {0};
	switch (f->format) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_YUVJ420P:
		out.pix = MC_PIX_I420;
		break;
	case AV_PIX_FMT_NV12:
		out.pix = MC_PIX_NV12;
		break;
	default:
		out.pix = MC_PIX_OTHER;
		break;
	}
	out.raw_format = f->format;
	out.full_range = f->color_range == AVCOL_RANGE_JPEG || f->format == AV_PIX_FMT_YUVJ420P;
	switch (f->colorspace) {
	case AVCOL_SPC_BT709:
		out.cs = MC_CS_709;
		break;
	case AVCOL_SPC_BT470BG:
	case AVCOL_SPC_SMPTE170M:
		out.cs = MC_CS_601;
		break;
	default:
		out.cs = MC_CS_UNKNOWN;
		break;
	}
	for (int i = 0; i < 3; i++) {
		out.data[i] = f->data[i];
		out.linesize[i] = f->linesize[i];
	}
	out.width = f->width;
	out.height = f->height;
	out.pts_us = f->pts != AV_NOPTS_VALUE ? f->pts : f->best_effort_timestamp;
	fn(opaque, &out);
}

static enum mc_dec_result dec_decode(struct mc_dec *d, const uint8_t *data, size_t len, int64_t pts, bool key,
				     mc_frame_fn fn, void *opaque)
{
	d->pkt->data = (uint8_t *)data;
	d->pkt->size = (int)len;
	d->pkt->pts = pts;
	d->pkt->dts = pts;
	d->pkt->flags = key ? AV_PKT_FLAG_KEY : 0;

	int r = avcodec_send_packet(d->ctx, d->pkt);
	if (r < 0 && r != AVERROR(EAGAIN))
		return MC_DEC_NEED_KEY;
	while (avcodec_receive_frame(d->ctx, d->frame) >= 0) {
		AVFrame *f = d->frame;
		if (f->format == MC_HW_PIX) {
			av_frame_unref(d->sw_frame);
			if (av_hwframe_transfer_data(d->sw_frame, f, 0) < 0) {
				av_frame_unref(f);
				return MC_DEC_HW_FAILED;
			}
			d->sw_frame->pts = f->pts;
			d->sw_frame->best_effort_timestamp = f->best_effort_timestamp;
			d->sw_frame->color_range = f->color_range;
			d->sw_frame->colorspace = f->colorspace;
			f = d->sw_frame;
		}
		emit(f, fn, opaque);
		av_frame_unref(d->frame);
	}
	return MC_DEC_OK;
}

static bool dec_has_hevc(void)
{
	return avcodec_find_decoder(AV_CODEC_ID_HEVC) != NULL;
}

static const struct mc_ffapi api = {
	.name = AVCODEC_NAME,
	.has_hevc = dec_has_hevc,
	.open = dec_open,
	.close = dec_close,
	.decode = dec_decode,
};

#ifdef _WIN32
static bool resolve(HMODULE lib, const char *name, void **out)
{
	*out = (void *)GetProcAddress(lib, name);
	if (!*out)
		LOG(LOG_WARNING, AVCODEC_NAME ": no %s", name);
	return *out != NULL;
}
#endif

/* load=false: беру библиотеку, только если OBS её уже загрузил; true: гружу сам из папки obs64.exe. */
const struct mc_ffapi *API_FN(bool load)
{
#ifdef _WIN32
	static bool ready;
	if (ready)
		return &api;
	HMODULE c = GetModuleHandleA(AVCODEC_NAME ".dll");
	HMODULE u = GetModuleHandleA(AVUTIL_NAME ".dll");
	/* только папка программы: чужая avcodec из PATH другой сборки FFmpeg мне не нужна */
	if (!c && load)
		c = LoadLibraryExA(AVCODEC_NAME ".dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!u && load)
		u = LoadLibraryExA(AVUTIL_NAME ".dll", NULL, LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!c || !u)
		return NULL;
	/* имя здесь только через # и ##, иначе препроцессор развернул бы его в ff.p_... из define выше */
#define RESOLVE_C(name)                                 \
	if (!resolve(c, #name, (void **)&ff.p_##name)) \
		return NULL;
#define RESOLVE_U(name)                                 \
	if (!resolve(u, #name, (void **)&ff.p_##name)) \
		return NULL;
	AVCODEC_FUNCS(RESOLVE_C)
	AVUTIL_FUNCS(RESOLVE_U)
	ready = true;
#else
	(void)load;
#endif
	return &api;
}
