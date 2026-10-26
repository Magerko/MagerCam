package com.magercam.app

import android.content.ContentValues
import android.content.Context
import android.media.MediaCodec
import android.media.MediaCodecInfo
import android.media.MediaFormat
import android.media.MediaMuxer
import android.net.Uri
import android.os.Build
import android.os.Environment
import android.os.ParcelFileDescriptor
import android.provider.MediaStore
import android.util.Log
import android.view.Surface
import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Пишу на телефон тот же кадр, что уходит в трансляцию, но отдельным кодером с высоким битрейтом:
 * трансляция может просесть из-за Wi-Fi, а запись остаётся в полном качестве для монтажа.
 */
class Recorder(private val ctx: Context) {
    private var video: MediaCodec? = null
    private var audio: MediaCodec? = null
    private var muxer: MediaMuxer? = null
    private var pfd: ParcelFileDescriptor? = null
    private var uri: Uri? = null
    private var file: File? = null
    private var videoTrack = -1
    private var audioTrack = -1
    private var muxerStarted = false
    private var lastVideoPts = -1L
    private var lastAudioPts = -1L
    private var drainThread: Thread? = null
    private val lock = Object()
    @Volatile private var running = false

    var inputSurface: Surface? = null
        private set
    @Volatile var startedAt = 0L
        private set
    @Volatile var lastFileName: String? = null
        private set

    val isRunning get() = running

    fun start(w: Int, h: Int, fps: Int, kbps: Int, withAudio: Boolean, orientation: Int): Boolean {
        stop()
        try {
            val vf = MediaFormat.createVideoFormat(MediaFormat.MIMETYPE_VIDEO_AVC, w, h).apply {
                setInteger(MediaFormat.KEY_COLOR_FORMAT, MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface)
                setInteger(MediaFormat.KEY_BIT_RATE, kbps * 1000)
                setInteger(MediaFormat.KEY_FRAME_RATE, fps)
                setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1)
            }
            val v = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_VIDEO_AVC)
            v.configure(vf, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
            inputSurface = v.createInputSurface()
            video = v

            if (withAudio) {
                val af = MediaFormat.createAudioFormat(MediaFormat.MIMETYPE_AUDIO_AAC, AudioCapture.RATE, AudioCapture.CHANNELS).apply {
                    setInteger(MediaFormat.KEY_AAC_PROFILE, MediaCodecInfo.CodecProfileLevel.AACObjectLC)
                    setInteger(MediaFormat.KEY_BIT_RATE, 128_000)
                    setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, AudioCapture.CHUNK_BYTES * 4)
                }
                audio = MediaCodec.createEncoderByType(MediaFormat.MIMETYPE_AUDIO_AAC).also {
                    it.configure(af, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE)
                }
            }

            val name = "MagerCam_" + SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".mp4"
            muxer = openMuxer(name)
            muxer!!.setOrientationHint(orientation)
            lastFileName = name
            videoTrack = -1; audioTrack = -1; muxerStarted = false
            lastVideoPts = -1; lastAudioPts = -1
            v.start()
            audio?.start()
            running = true
            startedAt = System.currentTimeMillis()
            drainThread = Thread({ drainLoop() }, "mc-rec").also { it.start() }
            Log.i(TAG, "recording $name ${w}x$h@$fps ${kbps}kbps audio=$withAudio")
            return true
        } catch (e: Exception) {
            Log.e(TAG, "start", e)
            release(false)
            return false
        }
    }

    private fun openMuxer(name: String): MediaMuxer {
        if (Build.VERSION.SDK_INT >= 29) {
            val values = ContentValues().apply {
                put(MediaStore.Video.Media.DISPLAY_NAME, name)
                put(MediaStore.Video.Media.MIME_TYPE, "video/mp4")
                put(MediaStore.Video.Media.RELATIVE_PATH, Environment.DIRECTORY_MOVIES + "/MagerCam")
                put(MediaStore.Video.Media.IS_PENDING, 1)
            }
            val u = ctx.contentResolver.insert(MediaStore.Video.Media.EXTERNAL_CONTENT_URI, values)
                ?: throw IllegalStateException("MediaStore insert failed")
            uri = u
            val fd = ctx.contentResolver.openFileDescriptor(u, "rw") ?: throw IllegalStateException("no fd")
            pfd = fd
            return MediaMuxer(fd.fileDescriptor, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
        }
        // до Android 10 без разрешения на память пишу в папку приложения: Android/data/com.magercam.app/files/Movies
        val dir = ctx.getExternalFilesDir(Environment.DIRECTORY_MOVIES) ?: ctx.filesDir
        val f = File(dir, name)
        file = f
        return MediaMuxer(f.absolutePath, MediaMuxer.OutputFormat.MUXER_OUTPUT_MPEG_4)
    }

    /** Звук приходит кусками по 20 мс с потока микрофона. */
    fun feedAudio(pcm: ByteArray, ptsUs: Long) {
        val a = audio ?: return
        if (!running) return
        try {
            val idx = a.dequeueInputBuffer(0)
            if (idx < 0) return
            val buf = a.getInputBuffer(idx) ?: return
            buf.clear()
            val n = minOf(pcm.size, buf.remaining())
            buf.put(pcm, 0, n)
            a.queueInputBuffer(idx, 0, n, ptsUs, 0)
        } catch (_: Exception) {}
    }

    private fun drainLoop() {
        val info = MediaCodec.BufferInfo()
        // любое исключение здесь уронило бы всё приложение, поэтому ловлю его и просто заканчиваю запись
        try {
            while (running) {
                val v = video ?: break
                drainOne(v, info, true, 10_000)
                audio?.let { drainOne(it, info, false, 0) }
            }
        } catch (e: Exception) {
            Log.e(TAG, "drain loop", e)
            running = false
        }
    }

    private fun drainOne(c: MediaCodec, info: MediaCodec.BufferInfo, isVideo: Boolean, timeoutUs: Long) {
        val idx = try { c.dequeueOutputBuffer(info, timeoutUs) } catch (_: Exception) { return }
        if (idx == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
            synchronized(lock) {
                val m = muxer ?: return
                // после старта muxer новые дорожки не принимает, а кодек при перенастройке камеры
                // может прислать формат повторно; такой повтор пропускаю
                if (muxerStarted || (if (isVideo) videoTrack else audioTrack) >= 0) return
                try {
                    if (isVideo) videoTrack = m.addTrack(c.outputFormat) else audioTrack = m.addTrack(c.outputFormat)
                    if (videoTrack >= 0 && (audio == null || audioTrack >= 0)) { m.start(); muxerStarted = true }
                } catch (e: Exception) {
                    Log.w(TAG, "muxer track: $e")
                }
            }
            return
        }
        if (idx < 0) return
        try {
            val buf = c.getOutputBuffer(idx)
            val config = info.flags and MediaCodec.BUFFER_FLAG_CODEC_CONFIG != 0
            if (buf != null && info.size > 0 && !config) {
                synchronized(lock) {
                    val track = if (isVideo) videoTrack else audioTrack
                    val last = if (isVideo) lastVideoPts else lastAudioPts
                    // muxer падает на немонотонных метках, поэтому такие сэмплы просто пропускаю
                    if (muxerStarted && track >= 0 && info.presentationTimeUs > last) {
                        buf.position(info.offset); buf.limit(info.offset + info.size)
                        muxer?.writeSampleData(track, buf, info)
                        if (isVideo) lastVideoPts = info.presentationTimeUs else lastAudioPts = info.presentationTimeUs
                    }
                }
            }
        } catch (e: Exception) {
            Log.w(TAG, "drain: $e")
        } finally {
            try { c.releaseOutputBuffer(idx, false) } catch (_: Exception) {}
        }
    }

    fun stop() {
        if (!running && video == null) return
        running = false
        drainThread?.join(1000)
        drainThread = null
        release(true)
    }

    private fun release(finish: Boolean) {
        running = false
        video?.let { try { it.stop() } catch (_: Exception) {}; try { it.release() } catch (_: Exception) {} }
        audio?.let { try { it.stop() } catch (_: Exception) {}; try { it.release() } catch (_: Exception) {} }
        video = null; audio = null
        inputSurface?.release(); inputSurface = null
        val ok = synchronized(lock) {
            val started = muxerStarted
            try { if (started) muxer?.stop() } catch (e: Exception) { Log.w(TAG, "muxer stop: $e") }
            try { muxer?.release() } catch (_: Exception) {}
            muxer = null; muxerStarted = false
            started && lastVideoPts > 0
        }
        try { pfd?.close() } catch (_: Exception) {}
        pfd = null
        uri?.let { u ->
            if (finish && ok) {
                ctx.contentResolver.update(u, ContentValues().apply { put(MediaStore.Video.Media.IS_PENDING, 0) }, null, null)
            } else {
                ctx.contentResolver.delete(u, null, null)
            }
        }
        if (!ok) file?.delete()
        uri = null; file = null
        startedAt = 0
        if (finish) Log.i(TAG, "recording stopped ok=$ok")
    }

    companion object {
        private const val TAG = "MC.Recorder"
    }
}
