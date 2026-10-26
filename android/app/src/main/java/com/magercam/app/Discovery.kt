package com.magercam.app

import android.content.Context
import android.net.wifi.WifiManager
import android.util.Log
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.InetSocketAddress

class Discovery(private val core: Core) {
    private var socket: DatagramSocket? = null
    private var thread: Thread? = null
    private var lock: WifiManager.MulticastLock? = null
    @Volatile private var running = false

    fun start() {
        if (running) return
        running = true
        try {
            val wifi = core.context.applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
            lock = wifi.createMulticastLock("magercam-discovery").apply { setReferenceCounted(false); acquire() }
        } catch (e: Exception) {
            Log.w(TAG, "multicast lock: $e")
        }
        thread = Thread({ loop() }, "mc-discovery").also { it.start() }
    }

    private fun loop() {
        while (running) {
            try {
                DatagramSocket(null).use { s ->
                    s.reuseAddress = true
                    s.broadcast = true
                    s.bind(InetSocketAddress(PORT))
                    socket = s
                    val buf = ByteArray(512)
                    while (running) {
                        val p = DatagramPacket(buf, buf.size)
                        s.receive(p)
                        val msg = String(p.data, 0, p.length)
                        if (msg.trim() != "MAGERCAM_DISCOVER") continue
                        val reply = JSONObject()
                            .put("magercam", 1)
                            .put("name", core.settings.displayName)
                            .put("port", core.server.port)
                            .toString().toByteArray()
                        s.send(DatagramPacket(reply, reply.size, p.socketAddress))
                    }
                }
            } catch (e: Exception) {
                if (running) {
                    Log.w(TAG, "discovery socket: $e")
                    Thread.sleep(2000)
                }
            }
        }
    }

    fun stop() {
        running = false
        socket?.close()
        try { lock?.release() } catch (_: Exception) {}
    }

    companion object {
        private const val TAG = "MC.Discovery"
        const val PORT = 4849
    }
}
