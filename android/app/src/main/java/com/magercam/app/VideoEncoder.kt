package com.magercam.app

import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaCodecList
import android.media.MediaFormat
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.Surface

class VideoEncoder(private val sink: (type: Int, packet: ByteArray, key: Boolean) -> Unit) {
    private var codec: MediaCodec? = null
    var inputSurface: Surface? = null
        private set
    private var drainThread: Thread? = null
    @Volatile private var running = false
    private var lastConfig: ByteArray? = null
    private var lastPts = Long.MIN_VALUE
    private var framesOut = 0L
    private var reorderSeen = false

    var width = 0; private set
    var height = 0; private set
    var fps = 0; private set
    var hevc = false; private set
    /** Кодер настроен с B-кадрами: кадры выходят не в порядке показа, клиенту нужен декодер с переупорядочиванием. */
    var bframes = false; private set
    /** Верхняя граница битрейта из возможностей кодера, кбит/с; 0 — не знаю. */
    var maxKbps = 0; private set

    val isRunning get() = running

    /**
     * [bitrateFor] получает кодек попытки и потолок битрейта этого кодера: авто-битрейт у H.265 ниже,
     * а узнать, какой кодек реально запустится, можно только перебором.
     * [quality]: VBR без приоритета реального времени; [allowBframes] — все клиенты умеют переупорядочивать кадры.
     */
    fun start(w: Int, h: Int, fps: Int, bitrateFor: (hevc: Boolean, maxKbps: Int) -> Int, wantHevc: Boolean = false,
              quality: Boolean = false, allowBframes: Boolean = false): Boolean {
        stop()
        // 2 — полный формат с B-кадрами, 1 — полный, 0 — минимальный: капризные кодеры не берут лишние ключи
        val levels = if (quality && allowBframes && Build.VERSION.SDK_INT >= 29) listOf(2, 1, 0) else listOf(1, 0)
        // HEVC при том же битрейте даёт картинку заметно лучше, но если кодер капризничает, откатываюсь на H.264
        val attempts = (if (wantHevc && isHevcSupported(w, h)) levels.map { true to it } else emptyList()) +
            levels.map { false to it }
        for ((useHevc, level) in attempts) {
            val mime = if (useHevc) MediaFormat.MIMETYPE_VIDEO_HEVC else MediaFormat.MIMETYPE_VIDEO_AVC
            val c = try {
                MediaCodec.createEncoderByType(mime)
            } catch (e: Exception) {
                Log.e(TAG, "no encoder for $mime", e); continue
            }
            val max = try {
                c.codecInfo.getCapabilitiesForType(mime).videoCapabilities.bitrateRange.upper / 1000
            } catch (_: Exception) { 0 }
            val kbps = bitrateFor(useHevc, max)
            try {
                val fmt = buildFormat(c, mime, w, h, fps, kbps, level, quality)
                c.configure(fmt, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                // многие кодеры молча игнорируют B-кадры; что они ответили, видно в формате, а что сделали — по pts в drain
                val bf = if (Build.VERSION.SDK_INT >= 29 && level == 2) try {
                    "in=${c.inputFormat.getInteger(MediaFormat.KEY_MAX_B_FRAMES, -1)} out=${c.outputFormat.getInteger(MediaFormat.KEY_MAX_B_FRAMES, -1)}"
                } catch (_: Exception) { "?" } else "0"
                inputSurface = c.createInputSurface()
                c.start()
                codec = c
                width = w; height = h; this.fps = fps; hevc = useHevc; bframes = level == 2; maxKbps = max
                lastConfig = null
                lastPts = Long.MIN_VALUE
                reorderSeen = false
                framesOut = 0
                running = true
                drainThread = Thread({ drain(c) }, "mc-encoder").also { it.start() }
                Log.i(TAG, "encoder started $mime ${w}x$h@$fps ${kbps}kbps max=${max}kbps level=$level " +
                    "quality=$quality mode=${modeName(fmt)} bframes=$bf (${c.name})")
                return true
            } catch (e: Exception) {
                Log.w(TAG, "encoder configure failed ($mime level=$level): $e")
                inputSurface?.release(); inputSurface = null
                try { c.release() } catch (_: Exception) {}
            }
        }
        return false
    }

    private fun buildFormat(c: MediaCodec, mime: String, w: Int, h: Int, fps: Int, kbps: Int, level: Int, quality: Boolean): MediaFormat {
        val f = MediaFormat.createVideoFormat(mime, w, h)
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        f.setInteger(MediaFormat.KEY_BIT_RATE, kbps * 1000)
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
        if (level == 0) return f
        // приоритет реального времени заставляет кодер экономить на анализе кадра, а в режиме «Качество» мне важнее детали
        if (!quality) f.setInteger(MediaFormat.KEY_PRIORITY, 0)
        if (Build.VERSION.SDK_INT >= 29) f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, if (level == 2) 2 else 0)
        f.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
        f.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
        f.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        try {
            val caps = c.codecInfo.getCapabilitiesForType(mime)
            val ec = caps.encoderCapabilities
            if (quality && ec.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR))
                f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR)
            else if (ec.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            if (mime == MediaFormat.MIMETYPE_VIDEO_HEVC) {
                f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.HEVCProfileMain)
            } else if (caps.profileLevels.any { it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh }) {
                f.setInteger(MediaFormat.KEY_PROFILE, MediaCodecInfo.CodecProfileLevel.AVCProfileHigh)
                f.setInteger(MediaFormat.KEY_LEVEL, levelFor(w, h, fps))
            }
        } catch (_: Exception) {}
        return f
    }

    private fun levelFor(w: Int, h: Int, fps: Int): Int {
        val mbps = ((w + 15) / 16) * ((h + 15) / 16) * fps
        return when {
            mbps <= 108000 -> MediaCodecInfo.CodecProfileLevel.AVCLevel31
            mbps <= 216000 -> MediaCodecInfo.CodecProfileLevel.AVCLevel4
            mbps <= 245760 -> MediaCodecInfo.CodecProfileLevel.AVCLevel41
            mbps <= 522240 -> MediaCodecInfo.CodecProfileLevel.AVCLevel42
            mbps <= 983040 -> MediaCodecInfo.CodecProfileLevel.AVCLevel51
            else -> MediaCodecInfo.CodecProfileLevel.AVCLevel52
        }
    }

    private fun emitConfig(cfg: ByteArray) {
        if (lastConfig?.contentEquals(cfg) == true) return
        lastConfig = cfg
        val pkt = StreamServer.packet(StreamServer.PKT_VIDEO_CONFIG, cfg, 0, true)
        if (hevc) pkt[3] = (pkt[3].toInt() or StreamServer.FLAG_HEVC).toByte()
        if (bframes) pkt[3] = (pkt[3].toInt() or StreamServer.FLAG_BFRAMES).toByte()
        sink(StreamServer.PKT_VIDEO_CONFIG, pkt, true)
    }

    private fun drain(c: MediaCodec) {
        val info = MediaCodec.BufferInfo()
        while (running) {
            val idx = try { c.dequeueOutputBuffer(info, 20_000) } catch (e: Exception) { break }
            if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                val fmt = c.outputFormat
                val sps = fmt.getByteBuffer("csd-0")
                val pps = fmt.getByteBuffer("csd-1")
                if (sps != null) {
                    // у HEVC в csd-0 уже лежат VPS+SPS+PPS, csd-1 нет
                    val out = ByteArray(sps.remaining() + (pps?.remaining() ?: 0))
                    sps.get(out, 0, sps.remaining())
                    pps?.get(out, out.size - pps.remaining(), pps.remaining())
                    emitConfig(out)
                }
            } else if (idx >= 0) {
                val buf = try { c.getOutputBuffer(idx) } catch (e: Exception) { null }
                if (buf != null && info.size > 0) {
                    buf.position(info.offset); buf.limit(info.offset + info.size)
                    if (info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0) {
                        val data = ByteArray(info.size)
                        buf.get(data)
                        emitConfig(data)
                    } else {
                        val key = info.flags and MediaCodec.BUFFER_FLAG_KEY_FRAME != 0
                        val pts = info.presentationTimeUs
                        if (bframes && !reorderSeen && lastPts != Long.MIN_VALUE && pts < lastPts) {
                            reorderSeen = true
                            Log.i(TAG, "b-frames confirmed: pts $pts after $lastPts")
                        }
                        lastPts = pts
                        if (bframes && ++framesOut == 300L && !reorderSeen) Log.i(TAG, "b-frames requested, but 300 frames came out in order")
                        val pkt = ByteArray(16 + info.size)
                        StreamServer.writeHeader(pkt, StreamServer.PKT_VIDEO_FRAME, info.size, pts, key)
                        buf.get(pkt, 16, info.size)
                        sink(StreamServer.PKT_VIDEO_FRAME, pkt, key)
                    }
                }
                try { c.releaseOutputBuffer(idx, false) } catch (_: Exception) { break }
            }
        }
    }

    fun setBitrate(kbps: Int) {
        try {
            codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_VIDEO_BITRATE, kbps * 1000) })
        } catch (e: Exception) { Log.w(TAG, "setBitrate: $e") }
    }

    fun requestKeyFrame() {
        try {
            codec?.setParameters(Bundle().apply { putInt(MediaCodec.PARAMETER_KEY_REQUEST_SYNC_FRAME, 0) })
        } catch (_: Exception) {}
    }

    fun stop() {
        running = false
        drainThread?.join(500)
        drainThread = null
        codec?.let {
            try { it.stop() } catch (_: Exception) {}
            try { it.release() } catch (_: Exception) {}
        }
        codec = null
        inputSurface?.release()
        inputSurface = null
    }

    companion object {
        private const val TAG = "MC.Encoder"

        private fun modeName(f: MediaFormat) = when (if (f.containsKey(MediaFormat.KEY_BITRATE_MODE)) f.getInteger(MediaFormat.KEY_BITRATE_MODE) else -1) {
            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_VBR -> "VBR"
            MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR -> "CBR"
            else -> "default"
        }

        private val avcEncoderCaps by lazy {
            try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                    .filter { info -> info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } }
                    .map { it.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities }
            } catch (_: Exception) { emptyList() }
        }

        private val hevcEncoderCaps by lazy {
            try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                    .filter { info -> info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_HEVC, true) } }
                    .filter { Build.VERSION.SDK_INT < 29 || it.isHardwareAccelerated }
                    .map { it.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_HEVC).videoCapabilities }
            } catch (_: Exception) { emptyList() }
        }

        fun isSizeSupported(w: Int, h: Int): Boolean =
            avcEncoderCaps.isEmpty() || avcEncoderCaps.any { it.isSizeSupported(w, h) }

        /** Сколько кадров в секунду аппаратный кодер заявляет для этого размера; если не заявляет ничего, не ограничиваю. */
        fun maxFps(w: Int, h: Int): Int {
            // замеры производителей бывают осторожными, а 1080p 60 тянут все живые телефоны, поэтому смотрю только выше 1080p
            if (w.toLong() * h <= 1920L * 1088) return 60
            val best = avcEncoderCaps.mapNotNull {
                try {
                    if (Build.VERSION.SDK_INT >= 23) it.getAchievableFrameRatesFor(w, h)?.upper ?: it.getSupportedFrameRatesFor(w, h).upper
                    else it.getSupportedFrameRatesFor(w, h).upper
                } catch (_: Exception) { null }
            }.maxOrNull() ?: return 60
            return best.toInt().coerceIn(15, 60)
        }

        /** Программный HEVC-кодер на телефоне слишком медленный для живого видео, поэтому беру только аппаратный. */
        fun isHevcSupported(w: Int = 1280, h: Int = 720): Boolean = hevcEncoderCaps.any { it.isSizeSupported(w, h) }
    }
}
