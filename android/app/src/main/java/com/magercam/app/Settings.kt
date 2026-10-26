package com.magercam.app

import android.content.Context
import android.content.SharedPreferences
import android.os.Build
import java.security.SecureRandom

/** Поля читаю и пишу из нескольких потоков, поэтому все они @Volatile. */
class Settings(context: Context) {
    private val prefs: SharedPreferences = context.getSharedPreferences("magercam", Context.MODE_PRIVATE)

    @Volatile var width = prefs.getInt("width", 1920)
    @Volatile var height = prefs.getInt("height", 1080)
    @Volatile var fps = prefs.getInt("fps", 30)
    @Volatile var bitrateKbps = prefs.getInt("bitrate", 8000)
    @Volatile var cameraId: String = prefs.getString("camera", "") ?: ""
    @Volatile var micEnabled = prefs.getBoolean("mic", true)
    @Volatile var audioSource = prefs.getString("audio_source", "camcorder") ?: "camcorder"
    @Volatile var port = prefs.getInt("port", 4848)
    @Volatile var deviceName: String = prefs.getString("name", "") ?: ""
    @Volatile var keepScreenOn = prefs.getBoolean("keep_screen_on", true)
    @Volatile var orientationMode = prefs.getString("orientation_mode", "auto") ?: "auto"

    @Volatile var torch = false
    @Volatile var zoom = prefs.getFloat("zoom", 1f)
    @Volatile var ev = prefs.getFloat("ev", 0f)
    @Volatile var focus = prefs.getString("focus", "continuous") ?: "continuous"
    @Volatile var focusDist = prefs.getFloat("focus_dist", 0f)
    @Volatile var wb = prefs.getString("wb", "auto") ?: "auto"
    @Volatile var antibanding = prefs.getString("antibanding", "auto") ?: "auto"
    @Volatile var stab = prefs.getBoolean("stab", false)
    @Volatile var aeLock = false
    @Volatile var awbLock = false
    @Volatile var mirrorPreview = prefs.getBoolean("mirror", false)

    @Volatile var authEnabled = prefs.getBoolean("auth", true)
    @Volatile var pin: String = prefs.getString("pin", null) ?: newPin().also { prefs.edit().putString("pin", it).apply() }
    @Volatile var profile = prefs.getString("profile", "") ?: ""
    @Volatile var abr = prefs.getBoolean("abr", true)
    @Volatile var codec = prefs.getString("codec", "auto") ?: "auto"
    @Volatile var framing = prefs.getBoolean("framing", false)
    @Volatile var background = prefs.getString("bg", "off") ?: "off"
    @Volatile var bgStrength = prefs.getFloat("bg_strength", 0.6f)
    @Volatile var heatGuard = prefs.getBoolean("heat_guard", true)
    @Volatile var autoDim = prefs.getBoolean("auto_dim", false)

    val displayName: String
        get() = deviceName.ifBlank { "${Build.MANUFACTURER.replaceFirstChar { it.uppercase() }} ${Build.MODEL}" }

    fun regeneratePin() {
        pin = newPin()
        save()
    }

    fun save() {
        prefs.edit()
            .putInt("width", width).putInt("height", height).putInt("fps", fps)
            .putInt("bitrate", bitrateKbps).putString("camera", cameraId)
            .putBoolean("mic", micEnabled).putString("audio_source", audioSource)
            .putInt("port", port).putString("name", deviceName)
            .putBoolean("keep_screen_on", keepScreenOn).putString("orientation_mode", orientationMode)
            .putFloat("zoom", zoom).putFloat("ev", ev).putString("focus", focus)
            .putFloat("focus_dist", focusDist).putString("wb", wb)
            .putString("antibanding", antibanding).putBoolean("stab", stab)
            .putBoolean("mirror", mirrorPreview)
            .putBoolean("auth", authEnabled).putString("pin", pin).putString("profile", profile)
            .putBoolean("abr", abr).putString("codec", codec).putBoolean("framing", framing)
            .putString("bg", background).putFloat("bg_strength", bgStrength)
            .putBoolean("heat_guard", heatGuard).putBoolean("auto_dim", autoDim)
            .apply()
    }

    companion object {
        private val rnd = SecureRandom()
        fun newPin(): String = "%06d".format(rnd.nextInt(1_000_000))
    }
}
