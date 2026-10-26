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
import java.net.InetSocketAddress
import java.net.NetworkInterface
import java.net.Socket
import java.util.concurrent.CopyOnWriteArrayList
import kotlin.math.abs

class Core(val context: Context) : CameraEngine.Listener {
    val settings = Settings(context)
    val server = StreamServer(this)
    val engine = CameraEngine(context, settings, this)
    private val audio = AudioCapture { pcm, pts ->
        server.broadcastAudio(pcm, pts)
        engine.recorder.feedAudio(pcm, pts)
    }
    private val discovery = Discovery(this)

    private val worker = HandlerThread("mc-core").apply { start() }
    private val handler = Handler(worker.looper)

    /** Колбэки UI дёргаю на рабочем потоке ядра, не на главном. */
    val uiListeners = CopyOnWriteArrayList<() -> Unit>()
    @Volatile var previewSizeListener: ((Size) -> Unit)? = null

    @Volatile var deviceOrientation = 0
        private set
    @Volatile var serverError: String? = null
        private set
    /** Битрейт, который сейчас реально отдаю: адаптивный режим держит его ниже настройки, пока сеть не тянет. */
    @Volatile var bitrateNow = settings.bitrateKbps
        private set
    @Volatile var overheated = false
        private set
    private var calmTicks = 0

    private val orientationListener = object : OrientationEventListener(context) {
        override fun onOrientationChanged(o: Int) {
            if (o == ORIENTATION_UNKNOWN) return
            val cur = deviceOrientation
            var diff = abs(o - cur) % 360
            if (diff > 180) diff = 360 - diff
            if (diff < 60) return // гистерезис, чтобы поворот не дёргался на границе 45°
            val snapped = ((o + 45) / 90 * 90) % 360
            if (snapped != cur) {
                deviceOrientation = snapped
                pushStatus()
            }
        }
    }

    private val statusTick = object : Runnable {
        override fun run() {
            adaptBitrate()
            checkHeat()
            pushStatus()
            handler.postDelayed(this, 1000)
        }
    }

    fun start() {
        engine.orientProvider = { streamOrientation() }
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

    fun onClientsChanged() {
        // HEVC включаю, только если его попросили все подключённые клиенты: кодер у них общий
        val hevc = server.streamClients.isNotEmpty() && server.streamClients.all { it.wantsHevc }
        if (hevc != engine.wantHevc) { engine.wantHevc = hevc; engine.restart() }
        engine.setStreamDemand(server.streamClients.isNotEmpty())
        engine.setMjpegDemand(server.wantsJpeg)
        updateAudio()
        notifyUi()
    }

    private fun updateAudio() {
        val want = settings.micEnabled && (server.streamClients.any { it.wantsAudio } || engine.isRecordingRequested)
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

    private fun targetBitrate(): Int =
        if (overheated && settings.heatGuard) minOf(settings.bitrateKbps, HOT_BITRATE) else settings.bitrateKbps

    /**
     * Адаптивный битрейт: если очередь на отправку растёт или кадры пришлось выкинуть, сразу сбавляю на 30%,
     * а поднимаю осторожно, только после нескольких спокойных секунд. Так картинка по слабому Wi-Fi
     * становится мягче, но не замирает.
     */
    private fun adaptBitrate() {
        val (drops, ageMs) = server.takeCongestion()
        val target = targetBitrate()
        var cur = bitrateNow
        if (!settings.abr || server.streamClients.isEmpty()) {
            cur = target
            calmTicks = 0
        } else if (drops > 0 || ageMs > 200) {
            cur = maxOf(MIN_BITRATE, (cur * 0.7f).toInt())
            calmTicks = 0
        } else if (ageMs < 60) {
            if (++calmTicks >= 4 && cur < target) {
                cur = (cur * 1.15f).toInt() + 100
                calmTicks = 2
            }
        }
        cur = cur.coerceAtMost(target)
        if (cur != bitrateNow) {
            bitrateNow = cur
            engine.bitrateOverride = cur
            engine.setBitrate(cur)
        }
    }

    /** Перегрев: при батарее горячее 43.5 °C снижаю частоту кадров до 30 и битрейт, отпускаю после 40.5 °C. */
    private fun checkHeat() {
        val temp = battery().third
        val hot = if (overheated) temp > 40.5f else temp >= 43.5f
        if (hot != overheated) {
            overheated = hot
            Log.i(TAG, "overheated=$hot temp=$temp")
        }
        val cap = if (overheated && settings.heatGuard) 30 else 0
        if (cap != engine.fpsCap) {
            engine.fpsCap = cap
            if (settings.fps > 30) engine.restart()
        }
    }

    /**
     * Сопряжение по QR: программа на ПК показывает код с адресом и одноразовым nonce, телефон сам
     * стучится к ней и передаёт свои адреса и код доступа. Так не нужно ни искать IP, ни вводить код руками.
     */
    fun pair(host: String, port: Int, nonce: String): String? = try {
        Socket().use { sock ->
            sock.connect(InetSocketAddress(host, port), 5000)
            sock.soTimeout = 5000
            val ips = JSONArray()
            ipAddresses().forEach { ips.put(it.second) }
            val json = JSONObject()
                .put("name", settings.displayName).put("ips", ips).put("port", settings.port)
                .put("key", if (settings.authEnabled) settings.pin else "").put("ver", PROTOCOL_VERSION)
            sock.getOutputStream().write("MCPAIR1 $nonce\n$json\n".toByteArray())
            sock.getOutputStream().flush()
            val reply = sock.getInputStream().bufferedReader().readLine()?.trim()
            if (reply == "OK") null else "ПК отклонил сопряжение: ${reply ?: "нет ответа"}"
        }
    } catch (e: Exception) {
        Log.w(TAG, "pair: $e")
        "Не удалось связаться с ПК $host: ${e.message}"
    }

    /** Ручная правка любого из этих значений означает, что профиль больше не действует. */
    private fun profileSnapshot() = listOf(settings.width, settings.height, settings.fps, settings.bitrateKbps,
        engine.resolveCameraId(settings.cameraId), settings.framing, settings.stab, settings.audioSource)

    /** Картинку для замены фона ужимаю до 1920 по большей стороне: больше кодеру всё равно не нужно. */
    fun saveBackground(bytes: ByteArray): Boolean {
        val opts = android.graphics.BitmapFactory.Options().apply { inJustDecodeBounds = true }
        android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size, opts)
        if (opts.outWidth <= 0) return false
        var sample = 1
        while (maxOf(opts.outWidth, opts.outHeight) / (sample * 2) >= 1920) sample *= 2
        val bmp = android.graphics.BitmapFactory.decodeByteArray(bytes, 0, bytes.size,
            android.graphics.BitmapFactory.Options().apply { inSampleSize = sample }) ?: return false
        val f = java.io.File(context.filesDir, GlRenderer.BG_FILE)
        f.outputStream().use { bmp.compress(android.graphics.Bitmap.CompressFormat.JPEG, 90, it) }
        bmp.recycle()
        if (settings.background == "off") applyParams(mapOf("bg" to "image")) else pushStatus()
        return true
    }

    private fun profileParams(name: String): Map<String, String>? = when (name) {
        "call" -> mapOf("w" to "1280", "h" to "720", "fps" to "30", "bitrate" to "3000", "camera" to "front",
            "framing" to "1", "stab" to "0", "audio_source" to "voice", "focus" to "continuous")
        "stream" -> mapOf("w" to "1920", "h" to "1080", "fps" to "60", "bitrate" to "12000", "camera" to "back",
            "framing" to "0", "stab" to "1", "audio_source" to "camcorder", "focus" to "continuous")
        "doc" -> mapOf("w" to "1920", "h" to "1080", "fps" to "30", "bitrate" to "8000", "camera" to "back",
            "framing" to "0", "stab" to "0", "audio_source" to "camcorder", "focus" to "continuous", "bg" to "off")
        else -> null
    }

    val clientCount get() = server.streamClients.size + server.mjpegClients.size

    /** Параметры /api/set и /stream описываю в docs/PROTOCOL.md. */
    fun applyParams(p: Map<String, String>) {
        if (p.isEmpty()) return
        p["profile"]?.let { name ->
            profileParams(name)?.let { preset ->
                applyParams(preset + (p - "profile"))
                settings.profile = name
                settings.save()
                return
            }
        }
        val before = profileSnapshot()
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
            if (v != settings.bitrateKbps) {
                settings.bitrateKbps = v
                bitrateNow = targetBitrate()
                engine.bitrateOverride = bitrateNow
                engine.setBitrate(bitrateNow)
            }
        }
        p["camera"]?.let {
            // на "back"/"front" оставляю текущий объектив, если он уже смотрит в эту сторону
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
        if ("abr" in p) settings.abr = b("abr")
        if ("heat_guard" in p) settings.heatGuard = b("heat_guard")
        p["codec"]?.let { if (it in listOf("auto", "h264") && it != settings.codec) { settings.codec = it; restart = true } }
        if ("framing" in p) { settings.framing = b("framing"); controls = true }
        p["bg"]?.let { if (it in listOf("off", "blur", "image") && it != settings.background) { settings.background = it; restart = true } }
        f("bg_strength")?.let { settings.bgStrength = it.coerceIn(0f, 1f) }
        if ("lock" in p) engine.lockAll(b("lock"))
        if ("rec" in p && b("rec") != engine.isRecordingRequested) {
            engine.setRecording(b("rec"), settings.micEnabled, streamOrientation())
            audioChanged = true
        }
        if (profileSnapshot() != before) settings.profile = ""

        if (p["af"] == "trigger") {
            engine.focusAt(f("x") ?: 0.5f, f("y") ?: 0.5f)
        }

        settings.save()
        if (restart) engine.restart() else if (controls) engine.updateControls()
        if (audioChanged) updateAudio()
        pushStatus()
    }

    /** Отдаю поворот по часовой, который приёмник должен применить, чтобы картинка встала ровно. */
    fun streamOrientation(): Int {
        val dev = when (settings.orientationMode) {
            "landscape" -> return 0
            "portrait" -> 0
            else -> deviceOrientation
        }
        val ch = engine.characteristics(engine.resolveCameraId(settings.cameraId))
        val sensor = ch?.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        val front = ch?.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        // считаю так же, как Camera2 считает JPEG_ORIENTATION
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
        o.put("name", settings.displayName).put("ver", PROTOCOL_VERSION)
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
        val fpsList = JSONArray()
        engine.supportedFps.forEach { fpsList.put(it) }
        o.put("fps_list", fpsList)
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
        o.put("auth", settings.authEnabled).put("profile", settings.profile)
        o.put("codec", if (engine.encoderHevc) "hevc" else "h264").put("codec_pref", settings.codec)
        o.put("hevc", VideoEncoder.isHevcSupported())
        o.put("abr", settings.abr).put("bitrate_now", bitrateNow)
        o.put("lock", settings.aeLock && settings.awbLock)
        o.put("framing", settings.framing).put("framing_available", engine.framingAvailable).put("faces", engine.facesVisible)
        o.put("bg", settings.background).put("bg_strength", settings.bgStrength.toDouble())
        o.put("bg_image", java.io.File(context.filesDir, GlRenderer.BG_FILE).exists())
        engine.bgError?.let { o.put("bg_error", it) }
        o.put("rec", engine.recorder.isRunning)
        if (engine.recorder.isRunning) o.put("rec_ms", System.currentTimeMillis() - engine.recorder.startedAt)
        engine.recordError?.let { o.put("rec_error", it) }
        o.put("hot", overheated).put("heat_guard", settings.heatGuard)
        engine.error?.let { o.put("error", it) }
        serverError?.let { o.put("error", it) }
        val ips = JSONArray()
        ipAddresses().forEach { ips.put(it.second) }
        o.put("ips", ips).put("port", settings.port)
        return o.toString()
    }

    private fun pushStatus() {
        handler.post {
            engine.glOrient = streamOrientation()
            if (server.streamClients.isNotEmpty()) server.broadcastStatus(statusJson())
            notifyUiNow()
        }
    }

    private fun notifyUi() = handler.post { notifyUiNow() }

    private fun notifyUiNow() {
        for (l in uiListeners) try { l() } catch (e: Exception) { Log.w(TAG, "ui listener: $e") }
    }

    override fun onEncoderPacket(type: Int, packet: ByteArray, key: Boolean) =
        server.broadcastVideo(type, packet, key)

    override fun onJpeg(jpeg: ByteArray) = server.broadcastJpeg(jpeg)

    override fun onEngineStateChanged() = pushStatus()

    override fun onPreviewSizeChanged(size: Size) {
        previewSizeListener?.invoke(size)
    }

    companion object {
        private const val TAG = "MC.Core"
        const val PROTOCOL_VERSION = 2
        private const val MIN_BITRATE = 800
        private const val HOT_BITRATE = 6000

        @Volatile var instance: Core? = null
    }
}
