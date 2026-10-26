package com.magercam.app

import android.content.Context
import android.util.Log
import org.tensorflow.lite.DataType
import org.tensorflow.lite.Interpreter
import java.io.FileInputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.channels.FileChannel

/**
 * Отделяю человека от фона моделью MediaPipe selfie_segmenter (assets/selfie_segmenter.tflite, Apache 2.0).
 * Размеры входа и выхода читаю из самой модели, чтобы её можно было заменить, не трогая код.
 */
class BgSegmenter(ctx: Context) {
    private val interpreter: Interpreter
    val inW: Int
    val inH: Int
    val outW: Int
    val outH: Int
    private val outChannels: Int
    private val floatInput: Boolean
    private val input: ByteBuffer
    private val output: ByteBuffer
    val mask: ByteArray

    init {
        val fd = ctx.assets.openFd("selfie_segmenter.tflite")
        val model = FileInputStream(fd.fileDescriptor).channel.map(FileChannel.MapMode.READ_ONLY, fd.startOffset, fd.declaredLength)
        interpreter = Interpreter(model, Interpreter.Options().setNumThreads(2))
        val inT = interpreter.getInputTensor(0)
        val outT = interpreter.getOutputTensor(0)
        inH = inT.shape()[1]; inW = inT.shape()[2]
        outH = outT.shape()[1]; outW = outT.shape()[2]
        outChannels = if (outT.shape().size > 3) outT.shape()[3] else 1
        floatInput = inT.dataType() == DataType.FLOAT32
        input = ByteBuffer.allocateDirect(inT.numBytes()).order(ByteOrder.nativeOrder())
        output = ByteBuffer.allocateDirect(outT.numBytes()).order(ByteOrder.nativeOrder())
        mask = ByteArray(outW * outH)
        Log.i(TAG, "model in ${inW}x$inH float=$floatInput out ${outW}x$outH x$outChannels")
    }

    /** [rgba] — кадр inW×inH, строки сверху вниз. Результат в [mask]: 255 — человек, 0 — фон. */
    fun run(rgba: ByteBuffer) {
        input.rewind()
        rgba.rewind()
        val n = inW * inH
        if (floatInput) {
            for (i in 0 until n) {
                input.putFloat((rgba.get().toInt() and 0xFF) / 255f)
                input.putFloat((rgba.get().toInt() and 0xFF) / 255f)
                input.putFloat((rgba.get().toInt() and 0xFF) / 255f)
                rgba.get()
            }
        } else {
            for (i in 0 until n) {
                input.put(rgba.get()); input.put(rgba.get()); input.put(rgba.get()); rgba.get()
            }
        }
        input.rewind()
        output.rewind()
        interpreter.run(input, output)
        output.rewind()
        val fb = output.asFloatBuffer()
        for (i in 0 until outW * outH) {
            // у двухклассовой модели беру вероятность второго класса (человек)
            val p = if (outChannels >= 2) fb.get(i * outChannels + 1) else fb.get(i)
            mask[i] = (p.coerceIn(0f, 1f) * 255f).toInt().toByte()
        }
    }

    fun close() = interpreter.close()

    companion object {
        private const val TAG = "MC.Segmenter"
    }
}
