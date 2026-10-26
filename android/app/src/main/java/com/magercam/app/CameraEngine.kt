package com.magercam.app

import android.Manifest
import android.annotation.SuppressLint
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.SurfaceTexture
import android.graphics.YuvImage
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.MeteringRectangle
import android.hardware.camera2.params.OutputConfiguration
import android.media.Image
import android.media.ImageReader
import android.media.MediaRecorder
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.os.SystemClock
import android.util.Log
import android.util.Range
import android.util.Size
import android.view.Surface
import android.view.SurfaceHolder
import java.io.ByteArrayOutputStream
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * One selectable lens. [id] is the key used in settings and the protocol: a plain Camera2 id, or
 * "logicalId:physicalId" for a lens that is only reachable through a logical multi-camera.
 */
data class CameraInfo(
    val id: String,
    val front: Boolean,
    val label: String,
    val openId: String = id,
    val physicalId: String? = null,
)

/**
 * Owns the Camera2 device. Opens the camera only while somebody needs frames:
 * the on-screen preview, a /stream client (H.264 encoder) or a /video client (MJPEG).
 * All camera work happens on a dedicated handler thread.
 */
class CameraEngine(private val ctx: Context, private val settings: Settings, private val listener: Listener) {
    interface Listener {
        fun onEncoderPacket(type: Int, packet: ByteArray, key: Boolean)
        fun onJpeg(jpeg: ByteArray)
        fun onEngineStateChanged()
        fun onPreviewSizeChanged(size: Size)
    }

    private val manager = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
    private val thread = HandlerThread("mc-camera").apply { start() }
    val handler = Handler(thread.looper)
    private val jpegThread = HandlerThread("mc-jpeg").apply { start() }
    private val jpegHandler = Handler(jpegThread.looper)

    private val encoder = VideoEncoder { t, p, k -> listener.onEncoderPacket(t, p, k) }

    // demands (camera thread)
    private var streamDemand = false
    private var mjpegDemand = false
    private var previewSurface: Surface? = null
    private var previewSurfaceSize: Size? = null

    // camera state (camera thread)
    private var device: CameraDevice? = null
    private var deviceId: String? = null
    private var opening = false
    private var chars: CameraCharacteristics? = null
    // characteristics of the lens that produces the frames (differs from [chars] for a physical lens)
    private var streamChars: CameraCharacteristics? = null
    private var physicalId: String? = null
    private var session: CameraCaptureSession? = null
    private var sessionGen = 0
    private var sessionSurfaces: List<Surface> = emptyList()
    private var builder: CaptureRequest.Builder? = null
    private var imageReader: ImageReader? = null
    private var fpsRange: Range<Int>? = null
    private var afRegion: MeteringRectangle? = null
    private var dropPreview = false
    private var dropMjpeg = false
    private var lastJpegAt = 0L

    // published state (read from any thread)
    @Volatile var activeSize: Size? = null; private set
    @Volatile var activeFps = 0; private set
    @Volatile var desiredPreviewSize: Size? = null; private set
    @Volatile var error: String? = null; private set
    @Volatile var isOpen = false; private set
    @Volatile var isFront = false; private set
    @Volatile var sensorOrientation = 90; private set
    @Volatile var timestampBootTime = false; private set
    @Volatile var zoomMin = 1f; private set
    @Volatile var zoomMax = 1f; private set
    @Volatile var evMin = 0f; private set
    @Volatile var evMax = 0f; private set
    @Volatile var hasFlash = false; private set
    @Volatile var currentCameraId: String = ""; private set
    @Volatile var supportedSizes: List<Size> = emptyList(); private set

    val cameras: List<CameraInfo> by lazy { listCameras() }

    private val reconfigureRunnable = Runnable { doReconfigure() }

    // ---------------------------------------------------------------- public API

    fun setStreamDemand(on: Boolean) = handler.post {
        if (streamDemand != on) { streamDemand = on; requestReconfigure() }
    }

    fun setMjpegDemand(on: Boolean) = handler.post {
        if (mjpegDemand != on) { mjpegDemand = on; dropMjpeg = false; requestReconfigure() }
    }

    fun setPreviewSurface(surface: Surface, w: Int, h: Int) = handler.post {
        previewSurface = surface
        previewSurfaceSize = Size(w, h)
        dropPreview = false
        requestReconfigure()
    }

    /** Synchronously detaches the preview surface (must finish before surfaceDestroyed returns). */
    fun clearPreviewSurface() {
        val latch = CountDownLatch(1)
        handler.post {
            previewSurface = null
            previewSurfaceSize = null
            handler.removeCallbacks(reconfigureRunnable)
            doReconfigure()
            latch.countDown()
        }
        latch.await(2, TimeUnit.SECONDS)
    }

    /** Resolution / fps / camera changed. */
    fun restart() = handler.post { requestReconfigure() }

    fun updateControls() = handler.post { applyRepeating() }

    fun setBitrate(kbps: Int) = handler.post { encoder.setBitrate(kbps) }

    fun requestKeyFrame() = handler.post { encoder.requestKeyFrame() }

    /** Tap-to-focus. x,y are normalized coordinates in the (unrotated) sensor image. */
    fun focusAt(x: Float, y: Float) = handler.post {
        val ch = chars ?: return@post
        val s = session ?: return@post
        val b = builder ?: return@post
        val active = ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE) ?: return@post
        val area = regionBase(active)
        val cx = area.left + (x.coerceIn(0f, 1f) * area.width()).toInt()
        val cy = area.top + (y.coerceIn(0f, 1f) * area.height()).toInt()
        val half = (minOf(area.width(), area.height()) * 0.08f).toInt()
        val rect = Rect(
            (cx - half).coerceAtLeast(area.left), (cy - half).coerceAtLeast(area.top),
            (cx + half).coerceAtMost(area.right - 1), (cy + half).coerceAtMost(area.bottom - 1)
        )
        afRegion = MeteringRectangle(rect, MeteringRectangle.METERING_WEIGHT_MAX - 1)
        val afModes = ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        if (!afModes.contains(CameraMetadata.CONTROL_AF_MODE_AUTO)) return@post
        settings.focus = "auto"
        applyControls(b)
        try {
            b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_CANCEL)
            s.capture(b.build(), null, handler)
            b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_START)
            s.capture(b.build(), null, handler)
            b.set(CaptureRequest.CONTROL_AF_TRIGGER, CameraMetadata.CONTROL_AF_TRIGGER_IDLE)
            s.setRepeatingRequest(b.build(), null, handler)
        } catch (e: Exception) {
            Log.w(TAG, "focusAt: $e")
        }
        listener.onEngineStateChanged()
    }

    fun release() {
        val latch = CountDownLatch(1)
        handler.post {
            streamDemand = false; mjpegDemand = false; previewSurface = null
            doReconfigure()
            latch.countDown()
        }
        latch.await(3, TimeUnit.SECONDS)
        thread.quitSafely()
        jpegThread.quitSafely()
    }

    fun resolveCameraId(pref: String): String {
        val list = cameras
        return when (pref) {
            "back" -> list.firstOrNull { !it.front }?.id
            "front" -> list.firstOrNull { it.front }?.id
            else -> list.firstOrNull { it.id == pref }?.id
        } ?: list.firstOrNull { !it.front }?.id ?: list.firstOrNull()?.id ?: "0"
    }

    /** Characteristics of a lens; for "logical:physical" ids those of the physical camera. */
    fun characteristics(id: String): CameraCharacteristics? = try {
        manager.getCameraCharacteristics(id.substringAfter(':'))
    } catch (_: Exception) { null }

    // ---------------------------------------------------------------- internals

    private fun listCameras(): List<CameraInfo> {
        class Lens(val openId: String, val physId: String?, val ch: CameraCharacteristics, val logical: Boolean)
        val lenses = ArrayList<Lens>()
        try {
            val ids = manager.cameraIdList.toList()
            for (id in ids) {
                val c = manager.getCameraCharacteristics(id)
                if (c.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_EXTERNAL) continue
                val logical = Build.VERSION.SDK_INT >= 28 && c.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES)
                    ?.contains(CameraMetadata.REQUEST_AVAILABLE_CAPABILITIES_LOGICAL_MULTI_CAMERA) == true
                lenses += Lens(id, null, c, logical)
                if (!logical) continue
                // Lenses hidden behind a logical camera (typical for ultra-wide / tele on many phones)
                for (pid in c.physicalCameraIds) {
                    if (pid in ids || lenses.any { it.physId == pid }) continue
                    val pc = try { manager.getCameraCharacteristics(pid) } catch (_: Exception) { continue }
                    if (!supportsPhysicalStream(pc)) continue
                    lenses += Lens(id, pid, pc, false)
                }
            }
        } catch (e: Exception) {
            Log.e(TAG, "listCameras", e)
        }

        val out = ArrayList<CameraInfo>()
        for (front in listOf(false, true)) {
            val used = HashMap<String, Int>()
            for (l in lenses) {
                if ((l.ch.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT) != front) continue
                val eq = equivFocal(l.ch)
                val base = if (front) "Фронтальная" else "Задняя"
                var name = when {
                    l.logical -> "$base (авто, все объективы)"
                    eq != null && eq < 20f -> "$base широкоугольная"
                    eq != null && eq > 40f -> "$base телефото"
                    else -> base
                }
                val n = (used[name] ?: 0) + 1
                used[name] = n
                if (n > 1) name += " $n"
                if (eq != null && !l.logical) name += " (${eq.roundToInt()} мм)"
                val key = if (l.physId != null) "${l.openId}:${l.physId}" else l.openId
                out += CameraInfo(key, front, name, l.openId, l.physId)
            }
        }
        return out
    }

    private fun supportsPhysicalStream(ch: CameraCharacteristics): Boolean {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return false
        return !map.getOutputSizes(SurfaceTexture::class.java).isNullOrEmpty()
    }

    /** 35 mm-equivalent focal length, used to tell wide / main / tele lenses apart. */
    private fun equivFocal(ch: CameraCharacteristics): Float? {
        val focal = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_FOCAL_LENGTHS)?.firstOrNull() ?: return null
        val phys = ch.get(CameraCharacteristics.SENSOR_INFO_PHYSICAL_SIZE) ?: return null
        val diag = kotlin.math.sqrt(phys.width * phys.width + phys.height * phys.height)
        if (diag <= 0f) return null
        return focal * 43.27f / diag
    }

    private fun requestReconfigure() {
        handler.removeCallbacks(reconfigureRunnable)
        handler.post(reconfigureRunnable)
    }

    private fun closeSession() {
        sessionGen++
        session?.let { try { it.close() } catch (_: Exception) {} }
        session = null
        builder = null
        sessionSurfaces = emptyList()
    }

    private fun closeDevice() {
        closeSession()
        device?.let { try { it.close() } catch (_: Exception) {} }
        device = null
        deviceId = null
        isOpen = false
    }

    private fun closeReader() {
        imageReader?.close()
        imageReader = null
    }

    private fun doReconfigure() {
        val needCamera = streamDemand || mjpegDemand || previewSurface != null
        closeSession()
        if (!needCamera) {
            encoder.stop()
            closeReader()
            closeDevice()
            activeSize = null
            listener.onEngineStateChanged()
            return
        }

        val id = resolveCameraId(settings.cameraId)
        if (device == null || deviceId != id) {
            if (opening) return
            encoder.stop()
            closeReader()
            closeDevice()
            openDevice(id)
            return
        }
        val dev = device ?: return
        val ch = streamChars ?: return

        val size = chooseVideoSize(ch, settings.width, settings.height)
        val range = chooseFpsRange(ch, settings.fps)
        fpsRange = range
        val fps = minOf(settings.fps, range.upper)
        activeSize = size
        activeFps = fps

        if (streamDemand) {
            if (!encoder.isRunning || encoder.width != size.width || encoder.height != size.height || encoder.fps != fps) {
                if (!encoder.start(size.width, size.height, fps, settings.bitrateKbps)) {
                    error = "Кодировщик H.264 не запустился для ${size.width}x${size.height}"
                }
            }
        } else encoder.stop()

        if (mjpegDemand && !dropMjpeg) {
            val ms = chooseMjpegSize(ch, size)
            val r = imageReader
            if (r == null || r.width != ms.width || r.height != ms.height) {
                closeReader()
                imageReader = ImageReader.newInstance(ms.width, ms.height, ImageFormat.YUV_420_888, 3).also {
                    it.setOnImageAvailableListener({ reader -> onImage(reader) }, jpegHandler)
                }
            }
        } else closeReader()

        val wantPreview = choosePreviewSize(ch, size)
        if (wantPreview != desiredPreviewSize) {
            desiredPreviewSize = wantPreview
            listener.onPreviewSizeChanged(wantPreview)
        }

        val surfaces = ArrayList<Surface>()
        val ps = previewSurface
        if (ps != null && ps.isValid && previewSurfaceSize == wantPreview && !dropPreview) surfaces += ps
        if (encoder.isRunning) encoder.inputSurface?.let { surfaces += it }
        imageReader?.surface?.let { surfaces += it }
        if (surfaces.isEmpty()) {
            listener.onEngineStateChanged()
            return
        }

        val gen = sessionGen
        try {
            val callback = object : CameraCaptureSession.StateCallback() {
                override fun onConfigured(s: CameraCaptureSession) {
                    if (gen != sessionGen) { s.close(); return }
                    session = s
                    sessionSurfaces = surfaces
                    error = null
                    applyRepeating()
                    encoder.requestKeyFrame()
                    listener.onEngineStateChanged()
                }

                override fun onConfigureFailed(s: CameraCaptureSession) {
                    if (gen != sessionGen) return
                    Log.w(TAG, "session configure failed with ${surfaces.size} surfaces")
                    // Too many streams for this device: give up the least important consumer and retry.
                    when {
                        surfaces.size > 1 && ps != null && surfaces.contains(ps) && !dropPreview -> dropPreview = true
                        imageReader != null && surfaces.size > 1 -> dropMjpeg = true
                        else -> {
                            error = "Камера не поддерживает ${size.width}x${size.height}@$fps"
                            listener.onEngineStateChanged()
                            return
                        }
                    }
                    requestReconfigure()
                }
            }
            val phys = physicalId
            if (phys != null && Build.VERSION.SDK_INT >= 28) {
                val outs = surfaces.map { OutputConfiguration(it).apply { setPhysicalCameraId(phys) } }
                dev.createCaptureSessionByOutputConfigurations(outs, callback, handler)
            } else {
                @Suppress("DEPRECATION")
                dev.createCaptureSession(surfaces, callback, handler)
            }
        } catch (e: Exception) {
            Log.e(TAG, "createCaptureSession", e)
            error = "Ошибка камеры: ${e.message}"
            listener.onEngineStateChanged()
        }
    }

    @SuppressLint("MissingPermission")
    private fun openDevice(id: String) {
        if (ctx.checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
            error = "Нет разрешения на камеру"
            listener.onEngineStateChanged()
            return
        }
        opening = true
        val info = cameras.firstOrNull { it.id == id }
        val openId = info?.openId ?: id
        physicalId = info?.physicalId
        val ch = characteristics(openId)
        chars = ch
        streamChars = characteristics(id) ?: ch
        if (ch != null) readCaps(id, streamChars!!, ch)
        try {
            manager.openCamera(openId, object : CameraDevice.StateCallback() {
                override fun onOpened(d: CameraDevice) {
                    opening = false
                    device = d
                    deviceId = id
                    isOpen = true
                    error = null
                    Log.i(TAG, "camera $id opened")
                    requestReconfigure()
                }

                override fun onDisconnected(d: CameraDevice) {
                    Log.w(TAG, "camera $id disconnected")
                    opening = false
                    d.close()
                    if (device == d) { device = null; deviceId = null; isOpen = false; session = null }
                    error = "Камера занята другим приложением"
                    listener.onEngineStateChanged()
                    handler.postDelayed({ requestReconfigure() }, 3000)
                }

                override fun onError(d: CameraDevice, code: Int) {
                    Log.e(TAG, "camera $id error $code")
                    opening = false
                    d.close()
                    if (device == d) { device = null; deviceId = null; isOpen = false; session = null }
                    error = "Ошибка камеры ($code)"
                    listener.onEngineStateChanged()
                    handler.postDelayed({ requestReconfigure() }, 3000)
                }
            }, handler)
        } catch (e: Exception) {
            opening = false
            Log.e(TAG, "openCamera", e)
            error = "Не удалось открыть камеру: ${e.message}"
            listener.onEngineStateChanged()
            handler.postDelayed({ requestReconfigure() }, 3000)
        }
    }

    /** [sc]: the lens producing frames, [ch]: the opened device (same unless a physical lens is used). */
    private fun readCaps(id: String, sc: CameraCharacteristics, ch: CameraCharacteristics) {
        currentCameraId = id
        isFront = sc.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        sensorOrientation = sc.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        timestampBootTime = sc.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
            CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
        hasFlash = ch.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true ||
            sc.get(CameraCharacteristics.FLASH_INFO_AVAILABLE) == true
        if (physicalId != null) {
            // zoom of the logical camera would switch lenses again; a fixed lens has none
            zoomMin = 1f; zoomMax = 1f
        } else if (Build.VERSION.SDK_INT >= 30 && ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
            val r = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)!!
            zoomMin = r.lower; zoomMax = r.upper
        } else {
            zoomMin = 1f; zoomMax = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
        }
        val evRange = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        val step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0f
        evMin = (evRange?.lower ?: 0) * step
        evMax = (evRange?.upper ?: 0) * step
        supportedSizes = videoSizes(sc)
    }

    private fun videoSizes(ch: CameraCharacteristics): List<Size> {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        val sizes = map.getOutputSizes(MediaRecorder::class.java)?.toList()
            ?: map.getOutputSizes(SurfaceTexture::class.java)?.toList() ?: emptyList()
        return sizes.filter { it.width <= 3840 && it.height <= 2160 && it.width >= 320 }
            .sortedByDescending { it.width * it.height }
    }

    private fun aspect(s: Size) = s.width.toFloat() / s.height

    private fun chooseVideoSize(ch: CameraCharacteristics, w: Int, h: Int): Size {
        val sizes = videoSizes(ch).filter { VideoEncoder.isSizeSupported(it.width, it.height) }
        if (sizes.isEmpty()) return Size(w, h)
        sizes.firstOrNull { it.width == w && it.height == h }?.let { return it }
        val target = w.toFloat() / h
        val area = w * h
        val sameAspect = sizes.filter { abs(aspect(it) - target) < 0.02f }
        sameAspect.filter { it.width * it.height <= area }.maxByOrNull { it.width * it.height }?.let { return it }
        sameAspect.minByOrNull { it.width * it.height }?.let { return it }
        return sizes.minByOrNull { abs(it.width * it.height - area) }!!
    }

    private fun choosePreviewSize(ch: CameraCharacteristics, video: Size): Size {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return video
        val sizes = map.getOutputSizes(SurfaceHolder::class.java)?.toList() ?: return video
        val a = aspect(video)
        val limit = 1920 * 1080
        if (sizes.contains(video) && video.width * video.height <= limit) return video
        return sizes.filter { abs(aspect(it) - a) < 0.02f && it.width * it.height <= limit }
            .maxByOrNull { it.width * it.height } ?: video
    }

    private fun chooseMjpegSize(ch: CameraCharacteristics, video: Size): Size {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return Size(640, 480)
        val sizes = map.getOutputSizes(ImageFormat.YUV_420_888)?.toList() ?: return Size(640, 480)
        val a = aspect(video)
        val limit = 1280 * 720
        return sizes.filter { abs(aspect(it) - a) < 0.02f && it.width * it.height <= limit }
            .maxByOrNull { it.width * it.height }
            ?: sizes.filter { it.width * it.height <= limit }.maxByOrNull { it.width * it.height }
            ?: sizes.minByOrNull { it.width * it.height }!!
    }

    private fun chooseFpsRange(ch: CameraCharacteristics, fps: Int): Range<Int> {
        val ranges = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return Range(fps, fps)
        ranges.firstOrNull { it.lower == fps && it.upper == fps }?.let { return it }
        ranges.filter { it.upper == fps }.maxByOrNull { it.lower }?.let { return it }
        ranges.filter { it.upper <= fps }.maxWithOrNull(compareBy({ it.upper }, { it.lower }))?.let { return it }
        return ranges.minByOrNull { abs(it.upper - fps) } ?: Range(fps, fps)
    }

    /** Area in which 3A regions are expressed: full active array with zoom ratio, crop region otherwise. */
    private fun regionBase(active: Rect): Rect {
        if (Build.VERSION.SDK_INT >= 30 && chars?.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null)
            return Rect(0, 0, active.width(), active.height())
        return cropFor(active, settings.zoom)
    }

    private fun cropFor(active: Rect, zoom: Float): Rect {
        val z = zoom.coerceIn(1f, zoomMax.coerceAtLeast(1f))
        val cw = (active.width() / z).toInt()
        val chh = (active.height() / z).toInt()
        val l = (active.width() - cw) / 2
        val t = (active.height() - chh) / 2
        return Rect(l, t, l + cw, t + chh)
    }

    private fun applyRepeating() {
        val dev = device ?: return
        val s = session ?: return
        try {
            val b = builder ?: dev.createCaptureRequest(
                if (encoder.isRunning) CameraDevice.TEMPLATE_RECORD else CameraDevice.TEMPLATE_PREVIEW
            ).also { nb -> sessionSurfaces.forEach { nb.addTarget(it) } }
            builder = b
            applyControls(b)
            s.setRepeatingRequest(b.build(), null, handler)
        } catch (e: Exception) {
            Log.w(TAG, "applyRepeating: $e")
        }
        listener.onEngineStateChanged()
    }

    private fun applyControls(b: CaptureRequest.Builder) {
        val ch = chars ?: return
        b.set(CaptureRequest.CONTROL_MODE, CameraMetadata.CONTROL_MODE_AUTO)
        fpsRange?.let { b.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }

        // exposure
        b.set(CaptureRequest.CONTROL_AE_MODE, CameraMetadata.CONTROL_AE_MODE_ON)
        b.set(CaptureRequest.FLASH_MODE,
            if (settings.torch && hasFlash) CameraMetadata.FLASH_MODE_TORCH else CameraMetadata.FLASH_MODE_OFF)
        val step = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP)?.toFloat() ?: 0f
        val evRange = ch.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE)
        if (step > 0f && evRange != null) {
            val steps = (settings.ev / step).roundToInt().coerceIn(evRange.lower, evRange.upper)
            b.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, steps)
        }
        b.set(CaptureRequest.CONTROL_AE_LOCK, settings.aeLock)
        val abModes = ch.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_ANTIBANDING_MODES) ?: intArrayOf()
        val ab = when (settings.antibanding) {
            "50" -> CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_50HZ
            "60" -> CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_60HZ
            "off" -> CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_OFF
            else -> CameraMetadata.CONTROL_AE_ANTIBANDING_MODE_AUTO
        }
        if (abModes.contains(ab)) b.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE, ab)

        // white balance
        val awbModes = ch.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES) ?: intArrayOf()
        val awb = when (settings.wb) {
            "daylight" -> CameraMetadata.CONTROL_AWB_MODE_DAYLIGHT
            "cloudy" -> CameraMetadata.CONTROL_AWB_MODE_CLOUDY_DAYLIGHT
            "shade" -> CameraMetadata.CONTROL_AWB_MODE_SHADE
            "incandescent" -> CameraMetadata.CONTROL_AWB_MODE_INCANDESCENT
            "fluorescent" -> CameraMetadata.CONTROL_AWB_MODE_FLUORESCENT
            "twilight" -> CameraMetadata.CONTROL_AWB_MODE_TWILIGHT
            else -> CameraMetadata.CONTROL_AWB_MODE_AUTO
        }
        b.set(CaptureRequest.CONTROL_AWB_MODE, if (awbModes.contains(awb)) awb else CameraMetadata.CONTROL_AWB_MODE_AUTO)
        b.set(CaptureRequest.CONTROL_AWB_LOCK, settings.awbLock)

        // focus
        val afModes = ch.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES) ?: intArrayOf()
        val minFocus = ch.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
        fun has(m: Int) = afModes.contains(m)
        when (settings.focus) {
            "auto" -> if (has(CameraMetadata.CONTROL_AF_MODE_AUTO)) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_AUTO)
            }
            "macro" -> if (has(CameraMetadata.CONTROL_AF_MODE_MACRO)) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_MACRO)
            }
            "infinity", "manual" -> if (minFocus > 0f) {
                b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_OFF)
                val d = if (settings.focus == "infinity") 0f else settings.focusDist.coerceIn(0f, 1f) * minFocus
                b.set(CaptureRequest.LENS_FOCUS_DISTANCE, d)
            }
            else -> when {
                has(CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO) ->
                    b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_VIDEO)
                has(CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE) ->
                    b.set(CaptureRequest.CONTROL_AF_MODE, CameraMetadata.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            }
        }
        val maxAf = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AF) ?: 0
        val region = afRegion
        if (maxAf > 0 && region != null && settings.focus == "auto") {
            b.set(CaptureRequest.CONTROL_AF_REGIONS, arrayOf(region))
        } else if (maxAf > 0) {
            b.set(CaptureRequest.CONTROL_AF_REGIONS, null)
        }
        val maxAe = ch.get(CameraCharacteristics.CONTROL_MAX_REGIONS_AE) ?: 0
        if (maxAe > 0) b.set(CaptureRequest.CONTROL_AE_REGIONS, if (region != null && settings.focus == "auto") arrayOf(region) else null)

        // zoom (not for a fixed physical lens)
        if (physicalId != null) {
            // leave the logical camera at its default zoom
        } else if (Build.VERSION.SDK_INT >= 30 && ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE) != null) {
            b.set(CaptureRequest.CONTROL_ZOOM_RATIO, settings.zoom.coerceIn(zoomMin, zoomMax))
        } else {
            ch.get(CameraCharacteristics.SENSOR_INFO_ACTIVE_ARRAY_SIZE)?.let {
                b.set(CaptureRequest.SCALER_CROP_REGION, cropFor(it, settings.zoom))
            }
        }

        // stabilization
        val vs = ch.get(CameraCharacteristics.CONTROL_AVAILABLE_VIDEO_STABILIZATION_MODES) ?: intArrayOf()
        if (vs.contains(CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON)) {
            b.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE,
                if (settings.stab) CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_ON
                else CameraMetadata.CONTROL_VIDEO_STABILIZATION_MODE_OFF)
        }
        val ois = ch.get(CameraCharacteristics.LENS_INFO_AVAILABLE_OPTICAL_STABILIZATION) ?: intArrayOf()
        if (ois.contains(CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON)) {
            b.set(CaptureRequest.LENS_OPTICAL_STABILIZATION_MODE, CameraMetadata.LENS_OPTICAL_STABILIZATION_MODE_ON)
        }
    }

    // ---------------------------------------------------------------- MJPEG

    private fun onImage(reader: ImageReader) {
        val img = try { reader.acquireLatestImage() } catch (_: Exception) { null } ?: return
        try {
            val now = SystemClock.elapsedRealtime()
            if (now - lastJpegAt < 45) return // ~20 fps max
            lastJpegAt = now
            val nv21 = toNv21(img)
            val out = ByteArrayOutputStream(img.width * img.height / 4)
            YuvImage(nv21, ImageFormat.NV21, img.width, img.height, null)
                .compressToJpeg(Rect(0, 0, img.width, img.height), 75, out)
            listener.onJpeg(out.toByteArray())
        } catch (e: Exception) {
            Log.w(TAG, "jpeg: $e")
        } finally {
            img.close()
        }
    }

    private fun toNv21(img: Image): ByteArray {
        val w = img.width
        val h = img.height
        val out = ByteArray(w * h * 3 / 2)
        val y = img.planes[0]
        val yBuf = y.buffer
        val yRow = y.rowStride
        if (y.pixelStride == 1) {
            for (r in 0 until h) {
                yBuf.position(r * yRow)
                yBuf.get(out, r * w, w)
            }
        } else {
            for (r in 0 until h) for (c in 0 until w) out[r * w + c] = yBuf.get(r * yRow + c * y.pixelStride)
        }
        val u = img.planes[1]
        val v = img.planes[2]
        val uBuf = u.buffer
        val vBuf = v.buffer
        val cRow = u.rowStride
        val cPix = u.pixelStride
        var o = w * h
        val ch = h / 2
        val cw = w / 2
        for (r in 0 until ch) {
            val base = r * cRow
            for (c in 0 until cw) {
                out[o++] = vBuf.get(r * v.rowStride + c * v.pixelStride)
                out[o++] = uBuf.get(base + c * cPix)
            }
        }
        return out
    }

    companion object {
        private const val TAG = "MC.Camera"
    }
}
