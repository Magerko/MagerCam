package com.magercam.app

import android.Manifest
import android.app.Activity
import android.app.AlertDialog
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.res.Configuration
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.hardware.camera2.CameraCharacteristics
import android.net.Uri
import android.os.SystemClock
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.util.Size
import android.util.TypedValue
import android.view.Gravity
import android.view.MotionEvent
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.ViewGroup
import android.view.WindowManager
import android.widget.AdapterView
import android.widget.ArrayAdapter
import android.widget.Button
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.Spinner
import android.widget.Switch
import android.widget.TextView
import android.widget.Toast
import kotlin.math.ln
import kotlin.math.exp
import kotlin.math.roundToInt

class MainActivity : Activity(), SurfaceHolder.Callback {
    private val main = Handler(Looper.getMainLooper())
    private var core: Core? = null

    private lateinit var root: FrameLayout
    private lateinit var aspect: AspectFrame
    private lateinit var surfaceView: SurfaceView
    private lateinit var focusRing: View
    private lateinit var dimOverlay: TextView
    private lateinit var tvAddr: TextView
    private lateinit var tvInfo: TextView
    private lateinit var tvErr: TextView
    private lateinit var btnTorch: TextView
    private lateinit var btnMic: TextView
    private lateinit var btnFocus: TextView
    private lateinit var btnRec: TextView
    private lateinit var btnFrame: TextView
    private lateinit var btnLock: TextView
    private lateinit var scanOverlay: LinearLayout
    private lateinit var zoomBar: SeekBar
    private lateinit var zoomLabel: TextView
    private lateinit var evBar: SeekBar
    private lateinit var evLabel: TextView

    private var surfaceReady = false
    private var previewSize: Size? = null
    private var dimmed = false
    private var seeking = false
    private var scanning = false
    private var cameraBeforeScan: String? = null
    private var lastBadQr: String? = null
    private var pendingPair: Uri? = null
    private var lastTouch = SystemClock.elapsedRealtime()

    private val autoDimTick = object : Runnable {
        override fun run() {
            val c = core
            if (c != null && c.settings.autoDim && !dimmed && !scanning && c.clientCount > 0 &&
                SystemClock.elapsedRealtime() - lastTouch > 60_000) setDimmed(true)
            main.postDelayed(this, 5000)
        }
    }

    private val uiListener: () -> Unit = {
        main.removeCallbacks(refreshRunnable)
        main.post(refreshRunnable)
    }
    private val refreshRunnable = Runnable { refreshUi() }

    private val exitReceiver = object : BroadcastReceiver() {
        override fun onReceive(c: Context?, i: Intent?) = finish()
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        buildUi()
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(exitReceiver, IntentFilter(CameraService.ACTION_EXIT), RECEIVER_NOT_EXPORTED)
        } else {
            @Suppress("UnspecifiedRegisterReceiverFlag")
            registerReceiver(exitReceiver, IntentFilter(CameraService.ACTION_EXIT))
        }
        val need = mutableListOf(Manifest.permission.CAMERA, Manifest.permission.RECORD_AUDIO)
        if (Build.VERSION.SDK_INT >= 33) need += Manifest.permission.POST_NOTIFICATIONS
        val missing = need.filter { checkSelfPermission(it) != PackageManager.PERMISSION_GRANTED }
        if (missing.isEmpty()) startCore() else requestPermissions(missing.toTypedArray(), 1)
        handlePairIntent(intent)
        main.postDelayed(autoDimTick, 5000)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        handlePairIntent(intent)
    }

    override fun dispatchTouchEvent(ev: MotionEvent): Boolean {
        lastTouch = SystemClock.elapsedRealtime()
        return super.dispatchTouchEvent(ev)
    }

    /** Ссылку magercam://pair открывает и системная камера телефона, если отсканировать QR ею, а не кнопкой в приложении. */
    private fun handlePairIntent(i: Intent?) {
        val u = i?.data ?: return
        if (u.scheme == "magercam" && u.host == "pair") {
            if (core != null) doPair(u) else pendingPair = u
        }
    }

    override fun onRequestPermissionsResult(code: Int, perms: Array<out String>, results: IntArray) {
        if (checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            startCore()
        } else {
            tvErr.text = "Нужно разрешение на камеру. Откройте настройки приложения и разрешите доступ."
            tvErr.visibility = View.VISIBLE
        }
    }

    override fun onResume() {
        super.onResume()
        hideSystemUi()
    }

    override fun onDestroy() {
        core?.uiListeners?.remove(uiListener)
        core?.previewSizeListener = null
        try { unregisterReceiver(exitReceiver) } catch (_: Exception) {}
        main.removeCallbacksAndMessages(null)
        super.onDestroy()
    }

    @Deprecated("Deprecated in Java")
    override fun onBackPressed() {
        if (dimmed) { setDimmed(false); return }
        Toast.makeText(this, "MagerCam продолжает работать в фоне. Остановить — кнопка ✕ или в уведомлении.", Toast.LENGTH_LONG).show()
        @Suppress("DEPRECATION")
        super.onBackPressed()
    }

    private fun startCore() {
        if (Core.instance == null) startForegroundService(Intent(this, CameraService::class.java))
        waitForCore()
    }

    private fun waitForCore() {
        val c = Core.instance
        if (c == null) { main.postDelayed({ waitForCore() }, 100); return }
        if (core === c) return
        core = c
        c.uiListeners += uiListener
        c.previewSizeListener = { s -> main.post { applyPreviewSize(s) } }
        c.engine.desiredPreviewSize?.let { applyPreviewSize(it) }
        pendingPair?.let { pendingPair = null; doPair(it) }
        if (surfaceReady) {
            val h = surfaceView.holder
            c.engine.setPreviewSurface(h.surface, h.surfaceFrame.width(), h.surfaceFrame.height(), displayDegrees())
        }
        refreshUi()
    }

    override fun surfaceCreated(holder: SurfaceHolder) {}

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        surfaceReady = true
        core?.engine?.setPreviewSurface(holder.surface, width, height, displayDegrees())
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        surfaceReady = false
        core?.engine?.clearPreviewSurface()
    }

    private fun applyPreviewSize(s: Size) {
        previewSize = s
        surfaceView.holder.setFixedSize(s.width, s.height)
        aspect.ratio = if (relativeRotation() % 180 == 90) s.height.toFloat() / s.width else s.width.toFloat() / s.height
    }

    private fun displayDegrees(): Int {
        @Suppress("DEPRECATION")
        val r = windowManager.defaultDisplay.rotation
        return r * 90
    }

    private fun cameraChars(): CameraCharacteristics? {
        val c = core ?: return null
        return c.engine.characteristics(c.engine.resolveCameraId(c.settings.cameraId))
    }

    /** Насколько повернуть превью на экране относительно кадра сенсора. */
    private fun relativeRotation(): Int {
        val ch = cameraChars()
        val sensor = ch?.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 90
        val front = ch?.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        val d = displayDegrees()
        return if (front) (sensor + d) % 360 else (sensor - d + 360) % 360
    }

    private fun onPreviewTap(ev: MotionEvent) {
        val c = core ?: return
        var u = ev.x / surfaceView.width
        val v = ev.y / surfaceView.height
        val front = cameraChars()?.get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_FRONT
        if (front) u = 1 - u
        if (c.settings.mirrorPreview) u = 1 - u
        val (x, y) = when (relativeRotation()) {
            90 -> v to 1 - u
            180 -> 1 - u to 1 - v
            270 -> 1 - v to u
            else -> u to v
        }
        c.applyParams(mapOf("af" to "trigger", "x" to "%.3f".format(java.util.Locale.US, x), "y" to "%.3f".format(java.util.Locale.US, y)))
        showFocusRing(ev.x + aspect.left + surfaceView.left, ev.y + aspect.top + surfaceView.top)
    }

    private fun showFocusRing(x: Float, y: Float) {
        val s = dp(64)
        focusRing.translationX = x - s / 2
        focusRing.translationY = y - s / 2
        focusRing.alpha = 1f
        focusRing.scaleX = 1.3f; focusRing.scaleY = 1.3f
        focusRing.visibility = View.VISIBLE
        focusRing.animate().scaleX(1f).scaleY(1f).setDuration(200).withEndAction {
            focusRing.animate().alpha(0f).setStartDelay(700).setDuration(300).start()
        }.start()
    }

    private fun dp(v: Int) = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v.toFloat(), resources.displayMetrics)
    private fun dpi(v: Int) = dp(v).roundToInt()

    private fun roundButton(label: String, onClick: () -> Unit): TextView = TextView(this).apply {
        text = label
        setTextColor(Color.WHITE)
        textSize = 18f
        gravity = Gravity.CENTER
        background = GradientDrawable().apply { shape = GradientDrawable.OVAL; setColor(0x88000000.toInt()) }
        layoutParams = LinearLayout.LayoutParams(dpi(52), dpi(52)).apply { setMargins(dpi(4), dpi(4), dpi(4), dpi(4)) }
        setOnClickListener { onClick() }
    }

    private fun setActive(v: TextView, on: Boolean) {
        (v.background as GradientDrawable).setColor(if (on) 0xCCFFB300.toInt() else 0x88000000.toInt())
    }

    private fun buildUi() {
        window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        root = FrameLayout(this).apply { setBackgroundColor(Color.BLACK) }

        aspect = AspectFrame(this)
        surfaceView = SurfaceView(this)
        surfaceView.holder.addCallback(this)
        aspect.addView(surfaceView, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT, Gravity.CENTER))
        surfaceView.setOnTouchListener { _, ev ->
            if (ev.action == MotionEvent.ACTION_UP) onPreviewTap(ev)
            true
        }
        root.addView(aspect, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        focusRing = View(this).apply {
            background = GradientDrawable().apply {
                shape = GradientDrawable.OVAL; setStroke(dpi(2), 0xFFFFD54F.toInt()); setColor(Color.TRANSPARENT)
            }
            visibility = View.GONE
        }
        root.addView(focusRing, FrameLayout.LayoutParams(dpi(64), dpi(64)))

        val info = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dpi(12), dpi(8), dpi(12), dpi(8))
            background = GradientDrawable().apply { cornerRadius = dp(10); setColor(0x99000000.toInt()) }
        }
        val title = TextView(this).apply {
            text = "MagerCam"; setTextColor(0xFFFFB300.toInt()); textSize = 16f; typeface = Typeface.DEFAULT_BOLD
        }
        tvAddr = TextView(this).apply { setTextColor(Color.WHITE); textSize = 17f; typeface = Typeface.MONOSPACE }
        tvInfo = TextView(this).apply { setTextColor(0xFFCCCCCC.toInt()); textSize = 12f }
        tvErr = TextView(this).apply { setTextColor(0xFFFF6E6E.toInt()); textSize = 13f; visibility = View.GONE }
        info.addView(title); info.addView(tvAddr); info.addView(tvInfo); info.addView(tvErr)
        root.addView(info, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.TOP or Gravity.START).apply {
            setMargins(dpi(12), dpi(12), dpi(12), dpi(12))
        })

        val col = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; gravity = Gravity.CENTER }
        col.addView(roundButton("⇄") { switchCamera() })
        btnTorch = roundButton("🔦") { core?.applyParams(mapOf("torch" to "toggle")) }
        col.addView(btnTorch)
        btnMic = roundButton("🎤") { core?.let { it.applyParams(mapOf("mic" to if (it.settings.micEnabled) "0" else "1")) } }
        col.addView(btnMic)
        btnFocus = roundButton("AF") { chooseFocus() }
        col.addView(btnFocus)
        btnFrame = roundButton("👤") { core?.let { it.applyParams(mapOf("framing" to if (it.settings.framing) "0" else "1")) } }
        col.addView(btnFrame)
        btnLock = roundButton("🔒") {
            core?.let {
                val on = !(it.settings.aeLock && it.settings.awbLock)
                it.applyParams(mapOf("lock" to if (on) "1" else "0"))
                Toast.makeText(this, if (on) "Экспозиция, баланс белого и фокус зафиксированы" else "Автонастройка снова включена", Toast.LENGTH_SHORT).show()
            }
        }
        col.addView(btnLock)
        btnRec = roundButton("⏺") { core?.let { it.applyParams(mapOf("rec" to if (it.engine.isRecordingRequested) "0" else "1")) } }
        col.addView(btnRec)
        col.addView(roundButton("QR") { if (scanning) stopScan() else startScan() })
        col.addView(roundButton("🌙") { setDimmed(true) })
        col.addView(roundButton("⚙") { showSettings() })
        col.addView(roundButton("?") { showHelp() })
        col.addView(roundButton("✕") { exitApp() })
        val scrollCol = ScrollView(this).apply { addView(col); isVerticalScrollBarEnabled = false }
        val portraitUi = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        root.addView(scrollCol, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.END or Gravity.CENTER_VERTICAL).apply {
            // в портрете не залезаю на ползунки внизу во всю ширину; если места мало, колонка прокручивается
            setMargins(dpi(8), dpi(8), dpi(8), dpi(if (portraitUi) 150 else 8))
        })

        // в альбомной ползунки стоят рядом, а в портрете экран узкий, поэтому каждому даю
        // свою строку во всю ширину, чтобы в него было проще попасть пальцем
        val portrait = resources.configuration.orientation == Configuration.ORIENTATION_PORTRAIT
        val bottom = LinearLayout(this).apply {
            orientation = if (portrait) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dpi(12), dpi(if (portrait) 8 else 4), dpi(12), dpi(if (portrait) 8 else 4))
            background = GradientDrawable().apply { cornerRadius = dp(if (portrait) 14 else 10); setColor(0x88000000.toInt()) }
        }
        val labelSize = if (portrait) 15f else 13f
        val barPad = dpi(if (portrait) 14 else 4)
        zoomLabel = TextView(this).apply { setTextColor(Color.WHITE); textSize = labelSize; minWidth = dpi(if (portrait) 64 else 56) }
        zoomBar = SeekBar(this).apply { max = 1000; setPadding(dpi(16), barPad, dpi(16), barPad) }
        evLabel = TextView(this).apply { setTextColor(Color.WHITE); textSize = labelSize; minWidth = dpi(64) }
        evBar = SeekBar(this).apply { max = 100; setPadding(dpi(16), barPad, dpi(16), barPad) }
        for ((lbl, bar) in listOf(zoomLabel to zoomBar, evLabel to evBar)) {
            val row = LinearLayout(this).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.CENTER_VERTICAL }
            row.addView(lbl)
            row.addView(bar, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            bottom.addView(row, if (portrait) LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
                else LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        }
        root.addView(bottom, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.BOTTOM).apply {
            // в портрете колонка кнопок выше, так что ползункам отдаю всю ширину
            setMargins(dpi(12), dpi(8), dpi(if (portrait) 12 else 80), dpi(12))
        })
        zoomBar.setOnSeekBarChangeListener(seekListener { p ->
            val c = core ?: return@seekListener
            val z = zoomFromProgress(p, c)
            zoomLabel.text = "%.1fx".format(z)
            c.applyParams(mapOf("zoom" to "%.3f".format(java.util.Locale.US, z)))
        })
        evBar.setOnSeekBarChangeListener(seekListener { p ->
            val c = core ?: return@seekListener
            val e = c.engine.evMin + (c.engine.evMax - c.engine.evMin) * p / 100f
            evLabel.text = "EV %+.1f".format(e)
            c.applyParams(mapOf("ev" to "%.2f".format(java.util.Locale.US, e)))
        })

        dimOverlay = TextView(this).apply {
            setBackgroundColor(Color.BLACK)
            setTextColor(0xFF333333.toInt())
            gravity = Gravity.CENTER
            text = "MagerCam работает\nкоснитесь, чтобы включить экран"
            visibility = View.GONE
            setOnClickListener { setDimmed(false) }
        }
        scanOverlay = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            gravity = Gravity.CENTER_HORIZONTAL
            setPadding(dpi(16), dpi(12), dpi(16), dpi(12))
            background = GradientDrawable().apply { cornerRadius = dp(12); setColor(0xCC000000.toInt()) }
            visibility = View.GONE
            addView(TextView(this@MainActivity).apply {
                text = "Наведите камеру на QR-код\nв программе MagerCam на ПК"
                setTextColor(Color.WHITE); textSize = 16f; gravity = Gravity.CENTER
            })
            addView(Button(this@MainActivity).apply { text = "Отмена"; setOnClickListener { stopScan() } })
        }
        root.addView(scanOverlay, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT, Gravity.CENTER))

        root.addView(dimOverlay, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT))

        setContentView(root)
        refreshUi()
    }

    private fun seekListener(onChange: (Int) -> Unit) = object : SeekBar.OnSeekBarChangeListener {
        override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) { if (fromUser) onChange(p) }
        override fun onStartTrackingTouch(s: SeekBar) { seeking = true }
        override fun onStopTrackingTouch(s: SeekBar) { seeking = false }
    }

    private fun zoomFromProgress(p: Int, c: Core): Float {
        val lo = c.engine.zoomMin.coerceAtLeast(0.1f)
        val hi = c.engine.zoomMax.coerceAtLeast(lo)
        if (hi <= lo) return lo
        return exp(ln(lo) + (ln(hi) - ln(lo)) * p / 1000f)
    }

    private fun progressFromZoom(z: Float, c: Core): Int {
        val lo = c.engine.zoomMin.coerceAtLeast(0.1f)
        val hi = c.engine.zoomMax.coerceAtLeast(lo)
        if (hi <= lo) return 0
        return ((ln(z.coerceIn(lo, hi)) - ln(lo)) / (ln(hi) - ln(lo)) * 1000).roundToInt()
    }

    @Suppress("DEPRECATION")
    private fun hideSystemUi() {
        window.decorView.systemUiVisibility = (View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or View.SYSTEM_UI_FLAG_FULLSCREEN or
            View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
            View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or View.SYSTEM_UI_FLAG_LAYOUT_STABLE)
    }

    private fun setDimmed(on: Boolean) {
        dimmed = on
        dimOverlay.visibility = if (on) View.VISIBLE else View.GONE
        val lp = window.attributes
        lp.screenBrightness = if (on) 0.01f else WindowManager.LayoutParams.BRIGHTNESS_OVERRIDE_NONE
        window.attributes = lp
    }

    private fun refreshUi() {
        val c = core
        if (c == null) {
            tvAddr.text = "Запуск…"
            return
        }
        val s = c.settings
        val ips = c.ipAddresses()
        val wifi = ips.firstOrNull { it.first.startsWith("wlan") || it.first.startsWith("ap") || it.first.startsWith("swlan") }
        tvAddr.text = buildString {
            if (wifi != null) append("Wi-Fi IP: ${wifi.second}\n") else append("Нет Wi-Fi — используйте USB\n")
            append("Порт: ${s.port}")
            if (s.authEnabled) append("\nКод: ${s.pin.substring(0, 3)} ${s.pin.substring(3)}")
            ips.filter { it != wifi }.take(2).forEach { append("\n${it.first}: ${it.second}") }
        }
        val size = c.engine.activeSize
        val (bat, charging, temp) = c.battery()
        tvInfo.text = buildString {
            val n = c.server.streamClients.size
            append(if (n > 0) "● Трансляция: клиентов $n" else "○ Ожидание подключения")
            val v = c.server.mjpegClients.size
            if (v > 0) append(" · браузер: $v")
            append("\n")
            val codec = if (c.engine.encoderHevc) "HEVC" else "H.264"
            val br = if (c.server.streamClients.isNotEmpty()) c.bitrateNow else s.bitrateKbps
            if (size != null) append("${size.width}×${size.height} @${c.engine.activeFps} · $codec ${"%.1f".format(br / 1000f)} Мбит/с\n")
            else append("${s.width}×${s.height} @${s.fps} · ${s.bitrateKbps / 1000} Мбит/с\n")
            if (c.engine.recorder.isRunning) {
                val sec = (System.currentTimeMillis() - c.engine.recorder.startedAt) / 1000
                append("⏺ Запись %d:%02d\n".format(sec / 60, sec % 60))
            }
            append("🔋 $bat%"); if (charging) append(" ⚡"); append(" · ${"%.0f".format(temp)}°C")
        }
        val err = c.serverError ?: c.engine.error ?: c.engine.recordError ?: c.engine.recorder.lastError ?: c.engine.bgError
            ?: if (c.overheated && s.heatGuard) "Телефон перегрелся: снижаю до 30 к/с и 6 Мбит/с. Снимите чехол и уберите его от солнца." else null
        tvErr.text = err ?: ""
        tvErr.visibility = if (err != null) View.VISIBLE else View.GONE

        setActive(btnTorch, s.torch)
        btnTorch.alpha = if (c.engine.hasFlash || !c.engine.isOpen) 1f else 0.4f
        btnMic.text = if (s.micEnabled) "🎤" else "🔇"
        setActive(btnMic, !s.micEnabled)
        val rec = c.engine.recorder.isRunning
        btnRec.text = if (rec) "■" else "⏺"
        (btnRec.background as GradientDrawable).setColor(if (rec) 0xCCE53935.toInt() else 0x88000000.toInt())
        btnFrame.visibility = if (c.engine.framingAvailable) View.VISIBLE else View.GONE
        setActive(btnFrame, s.framing)
        setActive(btnLock, s.aeLock && s.awbLock)
        btnFocus.text = when (s.focus) { "continuous" -> "AF"; "auto" -> "AF•"; "infinity" -> "∞"; "macro" -> "🌼"; else -> "MF" }

        if (!seeking) {
            zoomBar.progress = progressFromZoom(s.zoom, c)
            zoomLabel.text = "%.1fx".format(s.zoom.coerceIn(c.engine.zoomMin, c.engine.zoomMax.coerceAtLeast(c.engine.zoomMin)))
            val range = c.engine.evMax - c.engine.evMin
            evBar.progress = if (range > 0) ((s.ev - c.engine.evMin) / range * 100).roundToInt() else 50
            evLabel.text = "EV %+.1f".format(s.ev)
        }
        surfaceView.scaleX = if (s.mirrorPreview) -1f else 1f
        if (s.keepScreenOn) window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        else window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        previewSize?.let { applyPreviewSizeAspectOnly(it) }
    }

    private fun applyPreviewSizeAspectOnly(s: Size) {
        val r = if (relativeRotation() % 180 == 90) s.height.toFloat() / s.width else s.width.toFloat() / s.height
        if (r != aspect.ratio) aspect.ratio = r
    }

    private fun switchCamera() {
        val c = core ?: return
        val cams = c.engine.cameras
        if (cams.size < 2) return
        val cur = c.engine.resolveCameraId(c.settings.cameraId)
        val i = cams.indexOfFirst { it.id == cur }
        val next = cams[(i + 1) % cams.size]
        Toast.makeText(this, next.label, Toast.LENGTH_SHORT).show()
        c.applyParams(mapOf("camera" to next.id))
    }

    private fun chooseFocus() {
        val c = core ?: return
        val values = arrayOf("continuous", "auto", "infinity", "macro", "manual")
        val labels = arrayOf("Непрерывный автофокус", "По касанию (фиксировать)", "Бесконечность", "Макро", "Ручной")
        AlertDialog.Builder(this)
            .setTitle("Фокус")
            .setSingleChoiceItems(labels, values.indexOf(c.settings.focus)) { d, which ->
                if (values[which] == "manual") {
                    d.dismiss(); manualFocusDialog()
                } else {
                    c.applyParams(mapOf("focus" to values[which]))
                    d.dismiss()
                }
            }
            .show()
    }

    private fun manualFocusDialog() {
        val c = core ?: return
        c.applyParams(mapOf("focus" to "manual"))
        val label = TextView(this).apply { setPadding(dpi(20), dpi(12), dpi(20), 0) }
        val bar = SeekBar(this).apply {
            max = 100
            progress = (c.settings.focusDist * 100).roundToInt()
            setPadding(dpi(20), dpi(8), dpi(20), dpi(8))
        }
        fun upd(p: Int) { label.text = "Дистанция: $p% (0 — бесконечность, 100 — ближе всего)" }
        upd(bar.progress)
        bar.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(s: SeekBar, p: Int, fromUser: Boolean) {
                upd(p); c.applyParams(mapOf("focus_dist" to "%.2f".format(java.util.Locale.US, p / 100f)))
            }
            override fun onStartTrackingTouch(s: SeekBar) {}
            override fun onStopTrackingTouch(s: SeekBar) {}
        })
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; addView(label); addView(bar) }
        AlertDialog.Builder(this).setTitle("Ручной фокус").setView(box).setPositiveButton("Готово", null).show()
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_BG || resultCode != RESULT_OK) return
        val uri = data?.data ?: return
        val c = core ?: return
        Thread {
            val ok = try {
                contentResolver.openInputStream(uri)?.use { c.saveBackground(it.readBytes()) } ?: false
            } catch (_: Exception) { false }
            main.post {
                if (ok) c.applyParams(mapOf("bg" to "image"))
                Toast.makeText(this, if (ok) "Фон заменён" else "Не удалось открыть картинку", Toast.LENGTH_SHORT).show()
            }
        }.start()
    }

    private fun startScan() {
        val c = core ?: return
        scanning = true
        lastBadQr = null
        // QR на экране ПК удобнее снимать задней камерой
        val cur = c.engine.resolveCameraId(c.settings.cameraId)
        if (c.engine.cameras.firstOrNull { it.id == cur }?.front == true) {
            cameraBeforeScan = cur
            c.applyParams(mapOf("camera" to "back"))
        }
        scanOverlay.visibility = View.VISIBLE
        c.engine.setQrScan { text -> main.post { onQr(text) } }
    }

    private fun stopScan() {
        scanning = false
        scanOverlay.visibility = View.GONE
        core?.engine?.setQrScan(null)
        cameraBeforeScan?.let { core?.applyParams(mapOf("camera" to it)) }
        cameraBeforeScan = null
    }

    private fun onQr(text: String) {
        if (!scanning) return
        val u = Uri.parse(text)
        if (u.scheme != "magercam" || u.host != "pair") {
            if (text != lastBadQr) Toast.makeText(this, "Это не QR-код MagerCam", Toast.LENGTH_SHORT).show()
            lastBadQr = text
            return
        }
        stopScan()
        doPair(u, confirmed = true)
    }

    /** Только IPv4-адрес цифрами из частных диапазонов, 100.64/10 (Tailscale) или link-local. Имена не принимаю. */
    private fun isLocalAddress(h: String): Boolean {
        val p = h.split('.').map { it.toIntOrNull() ?: return false }
        if (p.size != 4 || p.any { it !in 0..255 }) return false
        return p[0] == 10 || (p[0] == 172 && p[1] in 16..31) || (p[0] == 192 && p[1] == 168) ||
            (p[0] == 100 && p[1] in 64..127) || (p[0] == 169 && p[1] == 254)
    }

    /**
     * Сопряжение отдаёт ПК код доступа, поэтому шлю его только на адреса локальной сети: домашней, рабочей
     * или Tailscale. Ссылку magercam://pair может подсунуть любая страница или чужой QR, и без этой проверки
     * код ушёл бы на сервер в интернете. [confirmed] — ссылку отсканировали кнопкой в самом приложении.
     */
    private fun doPair(u: Uri, confirmed: Boolean = false) {
        val c = core ?: return
        val all = u.getQueryParameter("h")?.split(',')?.map { it.trim() }?.filter { it.isNotEmpty() } ?: emptyList()
        val hosts = all.filter { isLocalAddress(it) }
        val port = u.getQueryParameter("p")?.toIntOrNull()?.takeIf { it in 1..65535 } ?: 4850
        val nonce = u.getQueryParameter("n") ?: return
        if (hosts.isEmpty()) {
            AlertDialog.Builder(this).setTitle("Не получилось")
                .setMessage("В QR-коде нет адреса ПК из локальной сети${if (all.isNotEmpty()) ": " + all.joinToString() else ""}. " +
                    "Код доступа на такие адреса не отправляю.")
                .setPositiveButton("OK", null).show()
            return
        }
        if (!confirmed) {
            AlertDialog.Builder(this).setTitle("Сопрячь с ПК?")
                .setMessage("Телефон передаст код доступа компьютеру ${hosts.joinToString()}. " +
                    "Соглашайтесь, только если QR-код показывает ваша программа MagerCam.")
                .setNegativeButton("Отмена", null)
                .setPositiveButton("Сопрячь") { _, _ -> doPair(u, true) }
                .show()
            return
        }
        Toast.makeText(this, "Сопряжение с ПК…", Toast.LENGTH_SHORT).show()
        Thread {
            var err: String? = "В QR-коде нет адреса ПК"
            for (h in hosts) {
                err = c.pair(h, port, nonce)
                if (err == null) break
            }
            main.post {
                AlertDialog.Builder(this)
                    .setTitle(if (err == null) "Готово" else "Не получилось")
                    .setMessage(err ?: "Телефон сопряжён с ПК. Программа MagerCam подключится к нему сама.")
                    .setPositiveButton("OK", null).show()
            }
        }.start()
    }

    private fun exitApp() {
        stopService(Intent(this, CameraService::class.java))
        finishAndRemoveTask()
    }

    private fun showHelp() {
        val c = core
        val ip = c?.ipAddresses()?.firstOrNull()?.second ?: "IP телефона"
        val port = c?.settings?.port ?: 4848
        val text = """
            |Быстрее всего: нажмите «QR» здесь и наведите телефон на QR-код в программе MagerCam на ПК.
            |
            |Zoom, Discord, Teams (Windows 11)
            |Установите MagerCam для Windows, выберите телефон и нажмите «Подключить». В Zoom выберите камеру «MagerCam».
            |
            |OBS
            |1. Установите плагин MagerCam для OBS (файл install-obs-plugin.bat).
            |2. В OBS: Источники → + → «MagerCam (телефон)».
            |3. Выберите телефон из списка найденных или введите IP: $ip
            |
            |Код доступа: ${c?.settings?.pin ?: "—"}. Его спросят при подключении по Wi-Fi. По USB код не нужен.
            |
            |USB
            |Включите «Отладку по USB» в настройках разработчика, подключите кабель и выберите в плагине «USB (через ADB)».
            |
            |Браузер / любые программы
            |Веб-панель: http://$ip:$port/
            |MJPEG-видео: http://$ip:$port/video
            |Снимок: http://$ip:$port/shot.jpg
            |
            |Совет: включите зарядку и затемнение экрана (🌙) — так телефон греется меньше.
        """.trimMargin()
        AlertDialog.Builder(this).setTitle("Как подключиться").setMessage(text).setPositiveButton("OK", null).show()
    }

    private fun showSettings() {
        val c = core ?: return
        val s = c.settings
        val box = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(dpi(20), dpi(8), dpi(20), dpi(8)) }
        fun header(t: String) = box.addView(TextView(this).apply {
            text = t; textSize = 13f; setTextColor(0xFFFFB300.toInt()); setPadding(0, dpi(12), 0, dpi(2))
        })
        fun <T> spinner(title: String, labels: List<String>, values: List<T>, current: T): Spinner {
            header(title)
            val sp = Spinner(this)
            sp.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, labels)
            sp.setSelection(values.indexOf(current).coerceAtLeast(0))
            box.addView(sp)
            return sp
        }
        fun switch(title: String, on: Boolean): Switch = Switch(this).apply {
            text = title; isChecked = on; setPadding(0, dpi(8), 0, dpi(8)); box.addView(this)
        }

        val profValues = listOf("", "call", "stream", "doc")
        val profSp = spinner("Профиль", listOf("Свои настройки", "Созвон (720p, фронталка, слежение за лицом)", "Стрим (1080p 60, стабилизация)", "Документ (1080p, задняя)"), profValues, s.profile)

        val cams = c.engine.cameras
        val camSp = spinner("Камера", cams.map { it.label }, cams.map { it.id }, c.engine.resolveCameraId(s.cameraId))
        var sizes = c.engine.characteristics(c.engine.resolveCameraId(s.cameraId))?.let { sizesOf(it) } ?: emptyList()
        val resHeader = "Разрешение"
        header(resHeader)
        val resSp = Spinner(this)
        box.addView(resSp)
        fun fillSizes() {
            resSp.adapter = ArrayAdapter(this, android.R.layout.simple_spinner_dropdown_item, sizes.map { "${it.width}×${it.height}" })
            val idx = sizes.indexOfFirst { it.width == s.width && it.height == s.height }
            resSp.setSelection(if (idx >= 0) idx else sizes.indexOfFirst { it.width == 1920 && it.height == 1080 }.coerceAtLeast(0))
        }
        fillSizes()
        camSp.onItemSelectedListener = object : AdapterView.OnItemSelectedListener {
            override fun onItemSelected(p: AdapterView<*>?, v: View?, pos: Int, id: Long) {
                sizes = c.engine.characteristics(cams[pos].id)?.let { sizesOf(it) } ?: emptyList()
                fillSizes()
            }
            override fun onNothingSelected(p: AdapterView<*>?) {}
        }
        val fpsValues = listOf(15, 24, 25, 30, 50, 60)
        val fpsSp = spinner("Кадров в секунду", fpsValues.map { "$it" }, fpsValues, s.fps)

        // ползунком на 50 делений точно не попасть, поэтому даю готовые значения с подсказкой, что под что
        val brValues = (listOf(2000, 3000, 4000, 6000, 8000, 10000, 12000, 16000, 20000, 25000, 30000, 40000, 50000) + s.bitrateKbps)
            .distinct().sorted()
        val brSp = spinner("Битрейт (верхняя граница)", brValues.map { kb ->
            val mb = if (kb % 1000 == 0) "${kb / 1000}" else "%.1f".format(kb / 1000f)
            mb + " Мбит/с" + when (kb) {
                3000 -> " — 720p"
                8000 -> " — 1080p 30"
                12000 -> " — 1080p 60"
                25000 -> " — 4K 30"
                40000 -> " — 4K 60, нужен хороший Wi-Fi"
                else -> ""
            }
        }, brValues, s.bitrateKbps)

        val audioSrcValues = listOf("camcorder", "mic", "voice", "unprocessed")
        val audioSp = spinner("Источник звука", listOf("Видеокамера (по умолчанию)", "Микрофон", "Голос (шумоподавление)", "Без обработки"), audioSrcValues, s.audioSource)
        val wbValues = listOf("auto", "daylight", "cloudy", "shade", "incandescent", "fluorescent", "twilight")
        val wbSp = spinner("Баланс белого", listOf("Авто", "Дневной свет", "Облачно", "Тень", "Лампа накаливания", "Флуоресцентная", "Сумерки"), wbValues, s.wb)
        val abValues = listOf("auto", "50", "60", "off")
        val abSp = spinner("Антимерцание", listOf("Авто", "50 Гц (Россия/Европа)", "60 Гц", "Выкл"), abValues, s.antibanding)
        val codecValues = listOf("auto", "h264")
        val codecSp = spinner("Кодек", listOf(if (VideoEncoder.isHevcSupported()) "Авто: HEVC, если ПК его умеет" else "Авто (HEVC этот телефон не умеет)", "Только H.264"), codecValues, s.codec)
        val orValues = listOf("auto", "landscape", "portrait")
        val orSp = spinner("Ориентация картинки в OBS", listOf("Авто (по положению телефона)", "Всегда альбомная", "Всегда портретная"), orValues, s.orientationMode)

        header("Параметры")
        val swStab = switch("Стабилизация видео", s.stab)
        val swAe = switch("Зафиксировать экспозицию", s.aeLock)
        val swAwb = switch("Зафиксировать баланс белого", s.awbLock)
        val swMirror = switch("Зеркалить превью на телефоне", s.mirrorPreview)
        val swScreen = switch("Не гасить экран", s.keepScreenOn)
        val swDim = switch("Затемнять экран через минуту без касаний во время трансляции", s.autoDim)
        val swFrame = if (c.engine.framingAvailable) switch("Автокадрирование: камера следит за лицом", s.framing) else null
        val swAbr = switch("Адаптивный битрейт (подстраиваться под Wi-Fi)", s.abr)
        val swHeat = switch("Беречь телефон: при перегреве снижать нагрузку", s.heatGuard)

        header("Фон (размытие или замена; работает на телефоне)")
        val bgValues = listOf("off", "blur", "image")
        val bgSp = Spinner(this).apply {
            adapter = ArrayAdapter(this@MainActivity, android.R.layout.simple_spinner_dropdown_item, listOf("Как есть", "Размыть", "Заменить картинкой"))
            setSelection(bgValues.indexOf(s.background).coerceAtLeast(0))
        }
        box.addView(bgSp)
        val bgBar = SeekBar(this).apply { max = 100; progress = (s.bgStrength * 100).roundToInt() }
        box.addView(TextView(this).apply { text = "Сила размытия" })
        box.addView(bgBar)
        box.addView(Button(this).apply {
            text = "Выбрать картинку фона…"
            setOnClickListener {
                @Suppress("DEPRECATION")
                startActivityForResult(Intent(Intent.ACTION_GET_CONTENT).setType("image/*").addCategory(Intent.CATEGORY_OPENABLE), REQ_BG)
            }
        })

        header("Безопасность")
        val swAuth = switch("Спрашивать код доступа при подключении по Wi-Fi", s.authEnabled)
        box.addView(Button(this).apply {
            text = "Сменить код (сейчас ${s.pin})"
            setOnClickListener {
                s.regeneratePin()
                text = "Новый код: ${s.pin}"
                refreshUi()
            }
        })

        header("Имя устройства (видно в OBS)")
        val nameEt = EditText(this).apply { setText(s.deviceName); hint = s.displayName; box.addView(this) }
        header("Порт")
        val portEt = EditText(this).apply { setText(s.port.toString()); inputType = InputType.TYPE_CLASS_NUMBER; box.addView(this) }

        val scroll = ScrollView(this).apply { addView(box) }
        AlertDialog.Builder(this)
            .setTitle("Настройки")
            .setView(scroll)
            .setNegativeButton("Отмена", null)
            .setPositiveButton("Сохранить") { _, _ ->
                val p = HashMap<String, String>()
                val prof = profValues[profSp.selectedItemPosition]
                if (prof.isNotEmpty() && prof != s.profile) {
                    c.applyParams(mapOf("profile" to prof))
                    refreshUi()
                    return@setPositiveButton
                }
                if (cams.isNotEmpty()) p["camera"] = cams[camSp.selectedItemPosition].id
                sizes.getOrNull(resSp.selectedItemPosition)?.let { p["w"] = "${it.width}"; p["h"] = "${it.height}" }
                p["fps"] = "${fpsValues[fpsSp.selectedItemPosition]}"
                p["bitrate"] = "${brValues[brSp.selectedItemPosition]}"
                p["audio_source"] = audioSrcValues[audioSp.selectedItemPosition]
                p["wb"] = wbValues[wbSp.selectedItemPosition]
                p["antibanding"] = abValues[abSp.selectedItemPosition]
                p["orientation_mode"] = orValues[orSp.selectedItemPosition]
                p["stab"] = if (swStab.isChecked) "1" else "0"
                p["ae_lock"] = if (swAe.isChecked) "1" else "0"
                p["awb_lock"] = if (swAwb.isChecked) "1" else "0"
                p["mirror"] = if (swMirror.isChecked) "1" else "0"
                p["name"] = nameEt.text.toString().trim()
                p["codec"] = codecValues[codecSp.selectedItemPosition]
                p["bg"] = bgValues[bgSp.selectedItemPosition]
                p["bg_strength"] = "%.2f".format(java.util.Locale.US, bgBar.progress / 100f)
                swFrame?.let { p["framing"] = if (it.isChecked) "1" else "0" }
                p["abr"] = if (swAbr.isChecked) "1" else "0"
                p["heat_guard"] = if (swHeat.isChecked) "1" else "0"
                s.keepScreenOn = swScreen.isChecked
                s.autoDim = swDim.isChecked
                s.authEnabled = swAuth.isChecked
                c.applyParams(p)
                val newPort = portEt.text.toString().toIntOrNull()
                if (newPort != null && newPort in 1024..65535 && newPort != s.port) {
                    s.port = newPort
                    s.save()
                    c.startServer()
                }
                refreshUi()
            }
            .show()
    }

    private fun sizesOf(ch: CameraCharacteristics): List<Size> {
        val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) ?: return emptyList()
        val list = map.getOutputSizes(android.media.MediaRecorder::class.java)?.toList() ?: return emptyList()
        return list.filter { it.width in 320..3840 && it.height <= 2160 && VideoEncoder.isSizeSupported(it.width, it.height) }
            .distinct().sortedByDescending { it.width * it.height }
    }
}

private const val REQ_BG = 7

class AspectFrame(ctx: Context) : FrameLayout(ctx) {
    var ratio = 0f
        set(v) { field = v; requestLayout() }

    override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
        val w = MeasureSpec.getSize(widthMeasureSpec)
        val h = MeasureSpec.getSize(heightMeasureSpec)
        if (ratio <= 0f || w == 0 || h == 0) {
            super.onMeasure(widthMeasureSpec, heightMeasureSpec); return
        }
        var cw = w
        var ch = (w / ratio).roundToInt()
        if (ch > h) { ch = h; cw = (h * ratio).roundToInt() }
        for (i in 0 until childCount) {
            getChildAt(i).measure(MeasureSpec.makeMeasureSpec(cw, MeasureSpec.EXACTLY), MeasureSpec.makeMeasureSpec(ch, MeasureSpec.EXACTLY))
        }
        setMeasuredDimension(w, h)
    }
}
