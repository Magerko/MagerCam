package com.magercam.app

import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraMetadata
import android.os.BatteryManager
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Size
import android.view.OrientationEventListener
import org.json.JSONArray
import org.json.JSONObject
import java.net.Inet4Address
import java.net.NetworkInterface
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.abs

/** Glue between camera, encoder, audio, server and UI. Lives as long as [CameraService]. */
class Core(val context: Context) : CameraEngine.Listener {
    val settings = Settings(context)
    val server = StreamServer(this)
    val engine = CameraEngine(context, settings, this)
    private val audio = AudioCapture { pcm, pts -> server.broadcastAudio(pcm, pts) }
    private val discovery = Discovery(this)

    private val worker = HandlerThread("mc-core").apply { start() }
    private val handler = Handler(worker.looper)

    /** UI callbacks, invoked on the core worker thread. */
    val uiListeners = CopyOnWriteArrayList<() -> Unit>()
    @Volatile var previewSizeListener: ((Size) -> Unit)? = null

    @Volatile var deviceOrientation = 0
        private set
    @Volatile var serverError: String? = null
        private set

    private val orientationListener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(o: Int) {
            if (o == ORIENTATION_UNKNOWN) return
            val cur = deviceOrientation
            var diff = abs(o - cur) % 360
            if (diff > 180) diff = 360 - diff
            if (diff < 60) return // hysteresis
            val snapped = ((o + 45) / 90 * 90) % 360
            if (snapped != cur) {
                deviceOrientation = snapped
                pushStatus()
            }
        }
    }

    private val statusTick = object : Runnable {
        override fun run() {
            pushStatus()
            handler.postDelayed(this, 1000)
        }
    }

    fun start() {
        startServer()
        discovery.start()
        if (orientationListener.canDetectOrientation()) orientationListener.enable()
        handler.post(statusTick)
    }

    fun startServer() {
        try {
            server.start(settings.port)
            serverError = null
        } catch (e: Exception) {
            Log.e(TAG, "server start", e)
            serverError = "Порт ${settings.port} занят: ${e.message}"
        }
        notifyUi()
    }

    fun stop() {
        handler.removeCallbacksAndMessages(null)
        orientationListener.disable()
        discovery.stop()
        server.stop()
        audio.stop()
        engine.release()
        worker.quitSafely()
    }

    // ---------------------------------------------------------------- demand

    fun onClientsChanged() {
        engine.setStreamDemand(server.streamClients.isNotEmpty())
        engine.setMjpegDemand(server.wantsJpeg)
        updateAudio()
        notifyUi()
    }

    private fun updateAudio() {
        val want = settings.micEnabled && server.streamClients.any { it.wantsAudio }
        if (want && !audio.isRunning) {
            val ch = engine.characteristics(engine.resolveCameraId(settings.cameraId))
            val boot = ch?.get(CameraCharacteristics.SENSOR_INFO_TIMESTAMP_SOURCE) ==
                CameraMetadata.SENSOR_INFO_TIMESTAMP_SOURCE_REALTIME
            audio.start(settings.audioSource, boot)
        } else if (!want && audio.isRunning) {
            audio.stop()
        }
    }

    fun requestKeyFrame() = engine.requestKeyFrame()

    val clientCount get() = server.streamClients.size + server.mjpegClients.size

    // ---------------------------------------------------------------- settings

    /** Applies /api/set (and /stream) parameters. See docs/PROTOCOL.md. */
    fun applyParams(p: Map<String, String>) {
        if (p.isEmpty()) return
        var restart = false
        var controls = false
        var audioChanged = false
        fun f(k: String) = p[k]?.replace(',', '.')?.toFloatOrNull()
        fun b(k: String) = p[k] == "1" || p[k] == "true" || p[k] == "on"

        p["w"]?.toIntOrNull()?.let { if (it in 160..7680 && it != settings.width) { settings.width = it; restart = true } }
        p["h"]?.toIntOrNull()?.let { if (it in 120..4320 && it != settings.height) { settings.height = it; restart = true } }
        p["fps"]?.toIntOrNull()?.let {
            val v = it.coerceIn(5, 60)
            if (v != settings.fps) { settings.fps = v; restart = true }
        }
        p["bitrate"]?.toIntOrNull()?.let {
            val v = it.coerceIn(500, 50000)
            if (v != settings.bitrateKbps) { settings.bitrateKbps = v; engine.setBitrate(v) }
        }
        p["camera"]?.let {
            // "back"/"front" keep the currently chosen lens if it already faces that way
            val cur = engine.cameras.firstOrNull { c -> c.id == engine.resolveCameraId(settings.cameraId) }
            val keep = cur != null && ((it == "back" && !cur.front) || (it == "front" && cur.front))
            val id = if (keep) cur!!.id else engine.resolveCameraId(it)
            if (id != engine.resolveCameraId(settings.cameraId)) {
                settings.cameraId = id
                settings.torch = false
                restart = true
                audioChanged = true
            } else if (settings.cameraId.isEmpty()) {
                settings.cameraId = id
            }
        }
        if ("mic" in p) { settings.micEnabled = b("mic"); audioChanged = true }
        p["audio_source"]?.let { if (it != settings.audioSource) { settings.audioSource = it; audio.stop(); audioChanged = true } }
        p["torch"]?.let { settings.torch = if (it == "toggle") !settings.torch else b("torch"); controls = true }
        f("zoom")?.let { settings.zoom = it.coerceIn(0.1f, 100f); controls = true }
        f("ev")?.let { settings.ev = it.coerceIn(-10f, 10f); controls = true }
        p["focus"]?.let {
            if (it in listOf("continuous", "auto", "infinity", "macro", "manual")) { settings.focus = it; controls = true }
        }
        f("focus_dist")?.let { settings.focusDist = it.coerceIn(0f, 1f); controls = true }
        p["wb"]?.let { settings.wb = it; controls = true }
        p["antibanding"]?.let { settings.antibanding = it; controls = true }
        if ("stab" in p) { settings.stab = b("stab"); controls = true }
        if ("ae_lock" in p) { settings.aeLock = b("ae_lock"); controls = true }
        if ("awb_lock" in p) { settings.awbLock = b("awb_lock"); controls = true }
        if ("mirror" in p) settings.mirrorPreview = b("mirror")
        p["orientation_mode"]?.let { if (it in listOf("auto", "landscape", "portrait")) settings.orientationMode = it }
        p["name"]?.let { settings.deviceName = it.take(60) }

        if (p["af"] == "trigger") {
            engine.focusAt(f("x") ?: 0.5f, f("y") ?: 0.5f)
        }

        settings.save()
        if (restart) engine.restart() else if (controls) engine.updateControls()
        if (audioChanged) updateAudio()
        pushStatus()
    }

    // ---------------------------------------------------------------- status

    /** Clockwise rotation the receiver must apply to the stream to make it upright. */
    fun streamOrientation(): Int {
        val dev = when (settings.orientationMode) {
            "landscape" -> return 0
            "portrait" -> 0
            else -> deviceOrientation
        }
        val ch = engine.characteristics(engine.resolveCameraId(settings.cameraId))
        val sensor = ch?.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        val front = ch?.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        // Same formula as Camera2 JPEG_ORIENTATION
        return if (front) (sensor - dev + 360) % 360 else (sensor + dev) % 360
    }

    fun ipAddresses(): List<Pair<String, String>> {
        val out = ArrayList<Pair<String, String>>()
        try {
            for (ni in NetworkInterface.getNetworkInterfaces()) {
                if (!ni.isUp || ni.isLoopback) continue
                for (a in ni.inetAddresses) {
                    if (a is Inet4Address && !a.isLoopbackAddress) out += ni.name to (a.hostAddress ?: continue)
                }
            }
        } catch (_: Exception) {}
        // Wi-Fi first
        return out.sortedBy { if (it.first.startsWith("wlan")) 0 else if (it.first.startsWith("ap") || it.first.startsWith("swlan")) 1 else 2 }
    }

    fun battery(): Triple<Int, Boolean, Float> {
        val i: Intent? = context.registerReceiver(null, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
        val level = i?.getIntExtra(BatteryManager.EXTRA_LEVEL, -1) ?: -1
        val scale = i?.getIntExtra(BatteryManager.EXTRA_SCALE, 100) ?: 100
        val plugged = (i?.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0) ?: 0) != 0
        val temp = (i?.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0) ?: 0) / 10f
        return Triple(if (level >= 0) level * 100 / scale else -1, plugged, temp)
    }

    fun statusJson(): String {
        val (bat, charging, temp) = battery()
        val size = engine.activeSize
        val o = JSONObject()
        o.put("name", settings.displayName).put("ver", 1)
        o.put("w", size?.width ?: settings.width).put("h", size?.height ?: settings.height)
        o.put("fps", if (engine.activeFps > 0) engine.activeFps else settings.fps)
        o.put("bitrate", settings.bitrateKbps)
        o.put("camera", engine.resolveCameraId(settings.cameraId)).put("front", engine.isFront)
        val cams = JSONArray()
        engine.cameras.forEach { cams.put(JSONObject().put("id", it.id).put("front", it.front).put("label", it.label)) }
        o.put("cameras", cams)
        val sizes = JSONArray()
        engine.supportedSizes.forEach { sizes.put("${it.width}x${it.height}") }
        o.put("sizes", sizes)
        o.put("orient", streamOrientation()).put("orientation_mode", settings.orientationMode)
        o.put("audio", settings.micEnabled).put("audio_source", settings.audioSource)
        o.put("torch", settings.torch).put("has_flash", engine.hasFlash)
        o.put("zoom", settings.zoom.toDouble()).put("zoom_min", engine.zoomMin.toDouble()).put("zoom_max", engine.zoomMax.toDouble())
        o.put("ev", settings.ev.toDouble()).put("ev_min", engine.evMin.toDouble()).put("ev_max", engine.evMax.toDouble())
        o.put("focus", settings.focus).put("focus_dist", settings.focusDist.toDouble())
        o.put("wb", settings.wb).put("antibanding", settings.antibanding).put("stab", settings.stab)
        o.put("ae_lock", settings.aeLock).put("awb_lock", settings.awbLock).put("mirror", settings.mirrorPreview)
        o.put("battery", bat).put("charging", charging).put("temp", temp.toDouble())
        o.put("clients", server.streamClients.size).put("viewers", server.mjpegClients.size)
        o.put("camera_open", engine.isOpen)
        engine.error?.let { o.put("error", it) }
        serverError?.let { o.put("error", it) }
        val ips = JSONArray()
        ipAddresses().forEach { ips.put(it.second) }
        o.put("ips", ips).put("port", settings.port)
        return o.toString()
    }

    private fun pushStatus() {
        handler.post {
            if (server.streamClients.isNotEmpty()) server.broadcastStatus(statusJson())
            notifyUiNow()
        }
    }

    private fun notifyUi() = handler.post { notifyUiNow() }

    private fun notifyUiNow() {
        for (l in uiListeners) try { l() } catch (e: Exception) { Log.w(TAG, "ui listener: $e") }
    }

    // ---------------------------------------------------------------- engine listener

    override fun onEncoderPacket(type: Int, packet: ByteArray, key: Boolean) =
        server.broadcastVideo(type, packet, key)

    override fun onJpeg(jpeg: ByteArray) = server.broadcastJpeg(jpeg)

    override fun onEngineStateChanged() = pushStatus()

    override fun onPreviewSizeChanged(size: Size) {
        previewSizeListener?.invoke(size)
    }

    companion object {
        private const val TAG = "MC.Core"
        @Volatile var instance: Core? = null
    }
}
