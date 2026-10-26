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

    var width = 0; private set
    var height = 0; private set
    var fps = 0; private set

    val isRunning get() = running

    fun start(w: Int, h: Int, fps: Int, bitrateKbps: Int): Boolean {
        stop()
        val attempts = listOf(true, false)
        for (full in attempts) {
            val c = try {
                MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            } catch (e: Exception) {
                Log.e(TAG, "no AVC encoder", e); return false
            }
            try {
                c.configure(buildFormat(c, w, h, fps, bitrateKbps, full), null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                inputSurface = c.createInputSurface()
                c.start()
                codec = c
                width = w; height = h; this.fps = fps
                lastConfig = null
                running = true
                drainThread = Thread({ drain(c) }, "mc-encoder").also { it.start() }
                Log.i(TAG, "encoder started ${w}x$h@$fps ${bitrateKbps}kbps full=$full (${c.name})")
                return true
            } catch (e: Exception) {
                Log.w(TAG, "encoder configure failed (full=$full): $e")
                inputSurface?.release(); inputSurface = null
                try { c.release() } catch (_: Exception) {}
            }
        }
        return false
    }

    private fun buildFormat(c: MediaCodec, w: Int, h: Int, fps: Int, kbps: Int, full: Boolean): MediaFormat {
        val f = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h)
        f.setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
        f.setInteger(MediaFormat.KEY_BIT_RATE, kbps * 1000)
        f.setInteger(MediaFormat.KEY_FRAME_RATE, fps)
        f.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 2)
        if (!full) return f
        f.setInteger(MediaFormat.KEY_PRIORITY, 0)
        if (Build.VERSION.SDK_INT >= 29) f.setInteger(MediaFormat.KEY_MAX_B_FRAMES, 0)
        f.setInteger(MediaFormat.KEY_COLOR_STANDARD, MediaFormat.COLOR_STANDARD_BT709)
        f.setInteger(MediaFormat.KEY_COLOR_RANGE, MediaFormat.COLOR_RANGE_LIMITED)
        f.setInteger(MediaFormat.KEY_COLOR_TRANSFER, MediaFormat.COLOR_TRANSFER_SDR_VIDEO)
        try {
            val caps = c.codecInfo.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC)
            if (caps.encoderCapabilities.isBitrateModeSupported(MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR))
                f.setInteger(MediaFormat.KEY_BITRATE_MODE, MediaCodecInfo.EncoderCapabilities.BITRATE_MODE_CBR)
            if (caps.profileLevels.any { it.profile == MediaCodecInfo.CodecProfileLevel.AVCProfileHigh }) {
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
        sink(StreamServer.PKT_VIDEO_CONFIG, StreamServer.packet(StreamServer.PKT_VIDEO_CONFIG, cfg, 0, true), true)
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
                        val pkt = ByteArray(16 + info.size)
                        StreamServer.writeHeader(pkt, StreamServer.PKT_VIDEO_FRAME, info.size, info.presentationTimeUs, key)
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

        private val avcEncoderCaps by lazy {
            try {
                MediaCodecList(MediaCodecList.REGULAR_CODECS).codecInfos
                    .filter { info -> info.isEncoder && info.supportedTypes.any { it.equals(MediaFormat.MIMETYPE_VIDEO_AVC, true) } }
                    .map { it.getCapabilitiesForType(MediaFormat.MIMETYPE_VIDEO_AVC).videoCapabilities }
            } catch (_: Exception) { emptyList() }
        }

        fun isSizeSupported(w: Int, h: Int): Boolean =
            avcEncoderCaps.isEmpty() || avcEncoderCaps.any { it.isSizeSupported(w, h) }
    }
}
