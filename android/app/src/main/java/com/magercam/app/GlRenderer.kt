package com.magercam.app

import android.content.Context
import android.graphics.BitmapFactory
import android.graphics.SurfaceTexture
import android.opengl.EGL14
import android.opengl.EGLConfig
import android.opengl.EGLContext
import android.opengl.EGLDisplay
import android.opengl.EGLExt
import android.opengl.EGLSurface
import android.opengl.GLES11Ext
import android.opengl.GLES20
import android.opengl.GLUtils
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.view.Surface
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.nio.FloatBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Промежуточная GL-ступень для размытия и замены фона: камера пишет в SurfaceTexture, я смешиваю резкий кадр
 * с размытым (или с картинкой) по маске человека и рисую результат в кодер, запись и превью.
 * Пока фон выключен, этой ступени нет вовсе и камера пишет в кодер напрямую.
 */
class GlRenderer(private val ctx: Context, private val settings: Settings) : SurfaceTexture.OnFrameAvailableListener {
    private val thread = HandlerThread("mc-gl").apply { start() }
    private val handler = Handler(thread.looper)
    private val segExec = Executors.newSingleThreadExecutor()

    private var display: EGLDisplay = EGL14.EGL_NO_DISPLAY
    private var context: EGLContext = EGL14.EGL_NO_CONTEXT
    private var config: EGLConfig? = null
    private var pbuffer: EGLSurface = EGL14.EGL_NO_SURFACE

    private var oesTex = 0
    private var surfaceTexture: SurfaceTexture? = null
    var inputSurface: Surface? = null
        private set
    private var inW = 0
    private var inH = 0

    private class Out(val surface: Surface, val egl: EGLSurface, val w: Int, val h: Int)
    private val outputs = ArrayList<Out>()

    private var prog = 0
    private var progOes = 0
    private var progBlur = 0
    private var progDown = 0
    private val quad: FloatBuffer = ByteBuffer.allocateDirect(4 * 4 * 4).order(ByteOrder.nativeOrder()).asFloatBuffer().apply {
        put(floatArrayOf(-1f, -1f, 0f, 0f, 1f, -1f, 1f, 0f, -1f, 1f, 0f, 1f, 1f, 1f, 1f, 1f)); position(0)
    }

    private val fbo = IntArray(3)
    private val fboTex = IntArray(3) // 0 — вход сегментации, 1 и 2 — пинг-понг размытия
    private var blurW = 0
    private var blurH = 0
    private var maskTex = 0
    private var bgTex = 0
    private var bgW = 0
    private var bgH = 0
    private var bgStamp = -1L

    private var segmenter: BgSegmenter? = null
    private var segInput: ByteBuffer? = null
    @Volatile private var segBusy = false
    @Volatile private var maskReady = false
    private var hasMask = false
    private var maskCopy = ByteArray(0)

    /** Поворот по часовой, после которого кадр встаёт ровно; от него зависит, как видит кадр сегментатор. */
    @Volatile var orient = 0
    @Volatile var error: String? = null
        private set

    private fun sync(block: () -> Unit) {
        val latch = CountDownLatch(1)
        handler.post { try { block() } catch (e: Exception) { Log.e(TAG, "gl", e); error = "Ошибка обработки фона: ${e.message}" } finally { latch.countDown() } }
        latch.await(3, TimeUnit.SECONDS)
    }

    /** Отдаю поверхность, в которую камера должна писать кадры размера [w]×[h]. */
    fun start(w: Int, h: Int): Surface? {
        sync {
            if (display == EGL14.EGL_NO_DISPLAY) initEgl()
            if (w != inW || h != inH) {
                inW = w; inH = h
                surfaceTexture?.setDefaultBufferSize(w, h)
                allocFbos()
            }
        }
        return inputSurface
    }

    fun setOutputs(list: List<Surface>) = sync {
        if (display == EGL14.EGL_NO_DISPLAY) return@sync
        EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
        val keep = outputs.filter { o -> list.any { it === o.surface } && o.surface.isValid }
        for (o in outputs) if (o !in keep) EGL14.eglDestroySurface(display, o.egl)
        outputs.clear()
        outputs.addAll(keep)
        for (s in list) {
            if (outputs.any { it.surface === s } || !s.isValid) continue
            val egl = EGL14.eglCreateWindowSurface(display, config, s, intArrayOf(EGL14.EGL_NONE), 0)
            if (egl == null || egl == EGL14.EGL_NO_SURFACE) {
                Log.w(TAG, "eglCreateWindowSurface failed: 0x${Integer.toHexString(EGL14.eglGetError())}")
                continue
            }
            val wh = IntArray(2)
            EGL14.eglQuerySurface(display, egl, EGL14.EGL_WIDTH, wh, 0)
            EGL14.eglQuerySurface(display, egl, EGL14.EGL_HEIGHT, wh, 1)
            outputs += Out(s, egl, wh[0], wh[1])
        }
    }

    fun release() {
        sync {
            if (display == EGL14.EGL_NO_DISPLAY) return@sync
            EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
            for (o in outputs) EGL14.eglDestroySurface(display, o.egl)
            outputs.clear()
            surfaceTexture?.release(); surfaceTexture = null
            inputSurface?.release(); inputSurface = null
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT)
            EGL14.eglDestroySurface(display, pbuffer)
            EGL14.eglDestroyContext(display, context)
            EGL14.eglTerminate(display)
            display = EGL14.EGL_NO_DISPLAY
            inW = 0; inH = 0
        }
        segExec.execute { segmenter?.close(); segmenter = null }
        segExec.shutdown()
        thread.quitSafely()
    }

    private fun initEgl() {
        display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY)
        val ver = IntArray(2)
        EGL14.eglInitialize(display, ver, 0, ver, 1)
        val attrs = intArrayOf(
            EGL14.EGL_RED_SIZE, 8, EGL14.EGL_GREEN_SIZE, 8, EGL14.EGL_BLUE_SIZE, 8, EGL14.EGL_ALPHA_SIZE, 8,
            EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT,
            EGL14.EGL_SURFACE_TYPE, EGL14.EGL_WINDOW_BIT or EGL14.EGL_PBUFFER_BIT,
            EGL_RECORDABLE_ANDROID, 1, EGL14.EGL_NONE
        )
        val configs = arrayOfNulls<EGLConfig>(1)
        val num = IntArray(1)
        EGL14.eglChooseConfig(display, attrs, 0, configs, 0, 1, num, 0)
        config = configs[0] ?: throw IllegalStateException("no EGL config")
        context = EGL14.eglCreateContext(display, config, EGL14.EGL_NO_CONTEXT, intArrayOf(EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE), 0)
        pbuffer = EGL14.eglCreatePbufferSurface(display, config, intArrayOf(EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE), 0)
        EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)

        prog = program(VS, FS_COMPOSITE)
        progOes = program(VS, FS_OES)
        progBlur = program(VS, FS_BLUR)
        progDown = program(VS, FS_DOWN)

        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        oesTex = t[0]
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        val st = SurfaceTexture(oesTex)
        st.setOnFrameAvailableListener(this, handler)
        surfaceTexture = st
        inputSurface = Surface(st)
        maskTex = newTex()
        bgTex = newTex()

        try {
            val seg = BgSegmenter(ctx)
            segmenter = seg
            segInput = ByteBuffer.allocateDirect(seg.inW * seg.inH * 4).order(ByteOrder.nativeOrder())
        } catch (e: Throwable) {
            Log.e(TAG, "segmenter", e)
            error = "Модель сегментации не загрузилась: ${e.message}"
        }
    }

    private fun newTex(): Int {
        val t = IntArray(1)
        GLES20.glGenTextures(1, t, 0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, t[0])
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MIN_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_MAG_FILTER, GLES20.GL_LINEAR)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_S, GLES20.GL_CLAMP_TO_EDGE)
        GLES20.glTexParameteri(GLES20.GL_TEXTURE_2D, GLES20.GL_TEXTURE_WRAP_T, GLES20.GL_CLAMP_TO_EDGE)
        return t[0]
    }

    private fun allocFbos() {
        if (fbo[0] != 0) {
            GLES20.glDeleteFramebuffers(3, fbo, 0)
            GLES20.glDeleteTextures(3, fboTex, 0)
        }
        blurW = maxOf(64, inW / 4)
        blurH = maxOf(36, inH / 4)
        GLES20.glGenFramebuffers(3, fbo, 0)
        val seg = segmenter
        for (i in 0 until 3) {
            fboTex[i] = newTex()
            val w = if (i == 0) seg?.inW ?: 16 else blurW
            val h = if (i == 0) seg?.inH ?: 16 else blurH
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, w, h, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, null)
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo[i])
            GLES20.glFramebufferTexture2D(GLES20.GL_FRAMEBUFFER, GLES20.GL_COLOR_ATTACHMENT0, GLES20.GL_TEXTURE_2D, fboTex[i], 0)
        }
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)
    }

    override fun onFrameAvailable(st: SurfaceTexture) {
        if (display == EGL14.EGL_NO_DISPLAY) return
        try {
            EGL14.eglMakeCurrent(display, pbuffer, pbuffer, context)
            st.updateTexImage()
            if (outputs.isEmpty()) return
            drawFrame(st.timestamp)
        } catch (e: Exception) {
            Log.w(TAG, "frame: $e")
        }
    }

    private fun drawFrame(ts: Long) {
        val rot = rotationMap(orient)
        val mode = if (settings.background == "image") 2 else 1
        if (mode == 2) loadBackground()

        // 1. маска: забираю готовую и, если сегментатор свободен, отдаю ему новый кадр
        val seg = segmenter
        if (maskReady && seg != null) {
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, maskTex)
            GLES20.glPixelStorei(GLES20.GL_UNPACK_ALIGNMENT, 1)
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_LUMINANCE, seg.outW, seg.outH, 0,
                GLES20.GL_LUMINANCE, GLES20.GL_UNSIGNED_BYTE, ByteBuffer.wrap(maskCopy))
            maskReady = false
            hasMask = true
        }
        val input = segInput
        if (seg != null && input != null && !segBusy) {
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo[0])
            GLES20.glViewport(0, 0, seg.inW, seg.inH)
            drawOes(progOes, rot, IDENTITY)
            input.rewind()
            GLES20.glReadPixels(0, 0, seg.inW, seg.inH, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, input)
            segBusy = true
            segExec.execute {
                try {
                    seg.run(input)
                    if (maskCopy.size != seg.mask.size) maskCopy = ByteArray(seg.mask.size)
                    System.arraycopy(seg.mask, 0, maskCopy, 0, seg.mask.size)
                    maskReady = true
                } catch (e: Exception) {
                    Log.w(TAG, "segment: $e")
                } finally {
                    segBusy = false
                }
            }
        }

        // 2. размытый фон: уменьшаю кадр и прогоняю гауссом по горизонтали и вертикали
        if (mode == 1) {
            GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, fbo[1])
            GLES20.glViewport(0, 0, blurW, blurH)
            // уменьшаю в 4 раза с усреднением 16 пикселей, иначе на размытом фоне остаются полосы
            GLES20.glUseProgram(progDown)
            val half = 1.5f / minOf(inW, inH)
            GLES20.glUniform2f(GLES20.glGetUniformLocation(progDown, "uHalf"), half, half)
            drawOes(progDown, IDENTITY, IDENTITY)
            val strength = settings.bgStrength.coerceIn(0f, 1f)
            val passes = 1 + (strength * 4).toInt()
            val spread = 1f + strength * 2f
            repeat(passes) {
                blurPass(fboTex[1], fbo[2], spread / blurW, 0f)
                blurPass(fboTex[2], fbo[1], 0f, spread / blurH)
            }
        }
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, 0)

        // 3. итоговый кадр в каждый выход, с меткой времени камеры, чтобы звук не разъехался с видео
        val upright = if (orient % 180 == 0) inW.toFloat() / inH else inH.toFloat() / inW
        val img = if (bgH > 0) bgW.toFloat() / bgH else upright
        val bgScale = if (img > upright) floatArrayOf(upright / img, 1f) else floatArrayOf(1f, img / upright)
        for (o in outputs) {
            if (!EGL14.eglMakeCurrent(display, o.egl, o.egl, context)) continue
            GLES20.glViewport(0, 0, o.w, o.h)
            GLES20.glUseProgram(prog)
            bindQuad(prog)
            GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(prog, "uTex"), 1, false, RAW_FRAME, 0)
            GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(prog, "uMap"), 1, false, IDENTITY, 0)
            GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(prog, "uMask"), 1, false, rot, 0)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex)
            GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "uCam"), 0)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE1)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, fboTex[1])
            GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "uBlur"), 1)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE2)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, maskTex)
            GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "uMaskT"), 2)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE3)
            GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, bgTex)
            GLES20.glUniform1i(GLES20.glGetUniformLocation(prog, "uBg"), 3)
            GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "uMode"), mode.toFloat())
            GLES20.glUniform1f(GLES20.glGetUniformLocation(prog, "uHasMask"), if (hasMask) 1f else 0f)
            GLES20.glUniform2f(GLES20.glGetUniformLocation(prog, "uBgScale"), bgScale[0], bgScale[1])
            GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
            GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
            EGLExt.eglPresentationTimeANDROID(display, o.egl, ts)
            EGL14.eglSwapBuffers(display, o.egl)
        }
    }

    private fun drawOes(p: Int, map: FloatArray, mask: FloatArray) {
        GLES20.glUseProgram(p)
        bindQuad(p)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(p, "uTex"), 1, false, RAW_FRAME, 0)
        GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(p, "uMap"), 1, false, map, 0)
        GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(p, "uMask"), 1, false, mask, 0)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES11Ext.GL_TEXTURE_EXTERNAL_OES, oesTex)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(p, "uCam"), 0)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun blurPass(src: Int, dst: Int, dx: Float, dy: Float) {
        GLES20.glBindFramebuffer(GLES20.GL_FRAMEBUFFER, dst)
        GLES20.glUseProgram(progBlur)
        bindQuad(progBlur)
        GLES20.glUniformMatrix4fv(GLES20.glGetUniformLocation(progBlur, "uTex"), 1, false, IDENTITY4, 0)
        GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(progBlur, "uMap"), 1, false, IDENTITY, 0)
        GLES20.glUniformMatrix3fv(GLES20.glGetUniformLocation(progBlur, "uMask"), 1, false, IDENTITY, 0)
        GLES20.glActiveTexture(GLES20.GL_TEXTURE0)
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, src)
        GLES20.glUniform1i(GLES20.glGetUniformLocation(progBlur, "uSrc"), 0)
        GLES20.glUniform2f(GLES20.glGetUniformLocation(progBlur, "uStep"), dx, dy)
        GLES20.glDrawArrays(GLES20.GL_TRIANGLE_STRIP, 0, 4)
    }

    private fun bindQuad(p: Int) {
        val aPos = GLES20.glGetAttribLocation(p, "aPos")
        val aUv = GLES20.glGetAttribLocation(p, "aUv")
        quad.position(0)
        GLES20.glVertexAttribPointer(aPos, 2, GLES20.GL_FLOAT, false, 16, quad)
        GLES20.glEnableVertexAttribArray(aPos)
        quad.position(2)
        GLES20.glVertexAttribPointer(aUv, 2, GLES20.GL_FLOAT, false, 16, quad)
        GLES20.glEnableVertexAttribArray(aUv)
    }

    private fun loadBackground() {
        val f = File(ctx.filesDir, BG_FILE)
        val stamp = if (f.exists()) f.lastModified() else 0L
        if (stamp == bgStamp) return
        bgStamp = stamp
        val bmp = if (f.exists()) BitmapFactory.decodeFile(f.absolutePath) else null
        GLES20.glBindTexture(GLES20.GL_TEXTURE_2D, bgTex)
        if (bmp != null) {
            GLUtils.texImage2D(GLES20.GL_TEXTURE_2D, 0, bmp, 0)
            bgW = bmp.width; bgH = bmp.height
            bmp.recycle()
        } else {
            // картинки нет: заливаю нейтральным серым, а не оставляю мусор в текстуре
            val grey = ByteBuffer.allocateDirect(4).put(byteArrayOf(60, 62, 66, -1)).apply { position(0) }
            GLES20.glTexImage2D(GLES20.GL_TEXTURE_2D, 0, GLES20.GL_RGBA, 1, 1, 0, GLES20.GL_RGBA, GLES20.GL_UNSIGNED_BYTE, grey)
            bgW = 1; bgH = 1
        }
    }

    private fun program(vs: String, fs: String): Int {
        fun shader(type: Int, src: String): Int {
            val s = GLES20.glCreateShader(type)
            GLES20.glShaderSource(s, src)
            GLES20.glCompileShader(s)
            val ok = IntArray(1)
            GLES20.glGetShaderiv(s, GLES20.GL_COMPILE_STATUS, ok, 0)
            if (ok[0] == 0) throw IllegalStateException("shader: " + GLES20.glGetShaderInfoLog(s))
            return s
        }
        val p = GLES20.glCreateProgram()
        GLES20.glAttachShader(p, shader(GLES20.GL_VERTEX_SHADER, vs))
        GLES20.glAttachShader(p, shader(GLES20.GL_FRAGMENT_SHADER, fs))
        GLES20.glLinkProgram(p)
        val ok = IntArray(1)
        GLES20.glGetProgramiv(p, GLES20.GL_LINK_STATUS, ok, 0)
        if (ok[0] == 0) throw IllegalStateException("link: " + GLES20.glGetProgramInfoLog(p))
        return p
    }

    companion object {
        private const val TAG = "MC.Gl"
        private const val EGL_RECORDABLE_ANDROID = 0x3142
        const val BG_FILE = "background.jpg"

        private val IDENTITY = floatArrayOf(1f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 1f)
        /**
         * Матрицу SurfaceTexture не беру: для текстурных выходов камера сама поворачивает буфер на ориентацию
         * сенсора, а кодеру отдаёт кадр как есть. Мне нужен ровно тот кадр, что получил бы кодер, поэтому только
         * переворачиваю по вертикали (строка 0 буфера — верх картинки).
         */
        private val RAW_FRAME = floatArrayOf(1f, 0f, 0f, 0f, 0f, -1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 1f, 0f, 1f)
        private val IDENTITY4 = floatArrayOf(1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f, 0f, 0f, 0f, 0f, 1f)

        /**
         * Отображение между координатами кадра камеры (GL, ось v вверх) и ровного кадра (ось вниз, как у картинки).
         * Для поворотов на 0/90/180/270 оно совпадает в обе стороны, поэтому одна матрица служит
         * и для входа сегментатора, и для чтения маски. Матрица 3×3 по столбцам: x' = a·x + b·y + c, y' = d·x + e·y + f.
         */
        fun rotationMap(orient: Int): FloatArray {
            val (a, b, c, d, e, f) = when ((orient % 360 + 360) % 360) {
                90 -> M6(0f, 1f, 0f, 1f, 0f, 0f)
                180 -> M6(-1f, 0f, 1f, 0f, 1f, 0f)
                270 -> M6(0f, -1f, 1f, -1f, 0f, 1f)
                else -> M6(1f, 0f, 0f, 0f, -1f, 1f)
            }
            return floatArrayOf(a, d, 0f, b, e, 0f, c, f, 1f)
        }

        private data class M6(val a: Float, val b: Float, val c: Float, val d: Float, val e: Float, val f: Float)

        private const val VS = """
            attribute vec4 aPos;
            attribute vec2 aUv;
            uniform mat4 uTex;
            uniform mat3 uMap;
            uniform mat3 uMask;
            varying vec2 vCam;
            varying vec2 vUv;
            varying vec2 vMask;
            void main() {
                gl_Position = aPos;
                vec2 c = (uMap * vec3(aUv, 1.0)).xy;
                vCam = (uTex * vec4(c, 0.0, 1.0)).xy;
                vUv = aUv;
                vMask = (uMask * vec3(aUv, 1.0)).xy;
            }
        """

        private const val FS_OES = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES uCam;
            varying vec2 vCam;
            void main() { gl_FragColor = texture2D(uCam, vCam); }
        """

        private const val FS_DOWN = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES uCam;
            uniform vec2 uHalf;
            varying vec2 vCam;
            void main() {
                gl_FragColor = 0.25 * (texture2D(uCam, vCam + uHalf) + texture2D(uCam, vCam - uHalf) +
                    texture2D(uCam, vCam + vec2(uHalf.x, -uHalf.y)) + texture2D(uCam, vCam + vec2(-uHalf.x, uHalf.y)));
            }
        """

        private const val FS_BLUR = """
            precision mediump float;
            uniform sampler2D uSrc;
            uniform vec2 uStep;
            varying vec2 vUv;
            void main() {
                vec4 c = texture2D(uSrc, vUv) * 0.227027;
                c += (texture2D(uSrc, vUv + uStep * 1.384615) + texture2D(uSrc, vUv - uStep * 1.384615)) * 0.316216;
                c += (texture2D(uSrc, vUv + uStep * 3.230769) + texture2D(uSrc, vUv - uStep * 3.230769)) * 0.070270;
                gl_FragColor = c;
            }
        """

        private const val FS_COMPOSITE = """
            #extension GL_OES_EGL_image_external : require
            precision mediump float;
            uniform samplerExternalOES uCam;
            uniform sampler2D uBlur;
            uniform sampler2D uMaskT;
            uniform sampler2D uBg;
            uniform float uMode;
            uniform float uHasMask;
            uniform vec2 uBgScale;
            varying vec2 vCam;
            varying vec2 vUv;
            varying vec2 vMask;
            void main() {
                vec4 sharp = texture2D(uCam, vCam);
                float m = uHasMask > 0.5 ? smoothstep(0.3, 0.7, texture2D(uMaskT, vMask).r) : 1.0;
                vec4 bg = uMode > 1.5 ? texture2D(uBg, (vMask - 0.5) * uBgScale + 0.5) : texture2D(uBlur, vUv);
                gl_FragColor = mix(bg, sharp, m);
            }
        """
    }
}
