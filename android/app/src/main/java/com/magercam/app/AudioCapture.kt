package com.magercam.app

import android.annotation.SuppressLint
import android.media.AudioFormat
import android.media.AudioRecord
import android.media.AudioTimestamp
import android.media.MediaRecorder
import android.os.SystemClock
import android.util.Log

/** Метки времени звука беру с тех же часов, что и у кадров камеры (см. [useBootTime]), иначе звук уедет от видео. */
class AudioCapture(private val sink: (pcm: ByteArray, ptsUs: Long) -> Unit) {
    @Volatile private var running = false
    private var thread: Thread? = null

    val isRunning get() = running

    fun start(source: String, useBootTime: Boolean) {
        if (running) return
        running = true
        thread = Thread({ loop(source, useBootTime) }, "mc-audio").also { it.start() }
    }

    fun stop() {
        running = false
        thread?.join(1000)
        thread = null
    }

    @SuppressLint("MissingPermission")
    private fun loop(source: String, useBootTime: Boolean) {
        val src = when (source) {
            "mic" -> MediaRecorder.AudioSource.MIC
            "voice" -> MediaRecorder.AudioSource.VOICE_COMMUNICATION
            "unprocessed" -> MediaRecorder.AudioSource.UNPROCESSED
            else -> MediaRecorder.AudioSource.CAMCORDER
        }
        val minBuf = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
        val rec = try {
            AudioRecord(src, RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, CHUNK_BYTES * 8))
                .takeIf { it.state == AudioRecord.STATE_INITIALIZED }
                ?: AudioRecord(MediaRecorder.AudioSource.MIC, RATE, AudioFormat.CHANNEL_IN_MONO,
                    AudioFormat.ENCODING_PCM_16BIT, maxOf(minBuf, CHUNK_BYTES * 8))
        } catch (e: Exception) {
            Log.e(TAG, "AudioRecord failed", e); running = false; return
        }
        if (rec.state != AudioRecord.STATE_INITIALIZED) {
            Log.e(TAG, "AudioRecord not initialized"); rec.release(); running = false; return
        }
        val timebase = if (useBootTime) AudioTimestamp.TIMEBASE_BOOTTIME else AudioTimestamp.TIMEBASE_MONOTONIC
        val ts = AudioTimestamp()
        var framesRead = 0L
        try {
            rec.startRecording()
            Log.i(TAG, "audio started source=$source")
            while (running) {
                val buf = ByteArray(CHUNK_BYTES)
                var off = 0
                while (off < buf.size && running) {
                    val n = rec.read(buf, off, buf.size - off)
                    if (n <= 0) break
                    off += n
                }
                if (off < buf.size) {
                    if (!running) break
                    Thread.sleep(10)
                    continue
                }
                val chunkStart = framesRead
                framesRead += CHUNK_FRAMES
                val ptsNs = if (rec.getTimestamp(ts, timebase) == AudioRecord.SUCCESS) {
                    ts.nanoTime + (chunkStart - ts.framePosition) * 1_000_000_000L / RATE
                } else {
                    val now = if (useBootTime) SystemClock.elapsedRealtimeNanos() else System.nanoTime()
                    now - CHUNK_FRAMES * 1_000_000_000L / RATE
                }
                sink(buf, ptsNs / 1000)
            }
        } catch (e: Exception) {
            Log.e(TAG, "audio loop", e)
        } finally {
            try { rec.stop() } catch (_: Exception) {}
            rec.release()
            running = false
            Log.i(TAG, "audio stopped")
        }
    }

    companion object {
        private const val TAG = "MC.Audio"
        const val RATE = 48000
        const val CHANNELS = 1
        const val CHUNK_FRAMES = 960
        const val CHUNK_BYTES = CHUNK_FRAMES * 2 * CHANNELS
    }
}
