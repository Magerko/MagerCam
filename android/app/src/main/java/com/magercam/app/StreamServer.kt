package com.magercam.app

import android.util.Log
import java.io.BufferedOutputStream
import java.io.IOException
import java.io.InputStream
import java.io.OutputStream
import java.net.InetSocketAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.URLDecoder
import java.nio.ByteBuffer
import java.util.concurrent.CopyOnWriteArrayList
import java.util.concurrent.LinkedBlockingDeque
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicLong

class StreamServer(private val core: Core) {
    private var server: ServerSocket? = null
    private var acceptThread: Thread? = null
    @Volatile private var running = false

    val streamClients = CopyOnWriteArrayList<StreamClient>()
    val mjpegClients = CopyOnWriteArrayList<MjpegClient>()
    private val jpegWaiters = CopyOnWriteArrayList<JpegWaiter>()

    @Volatile var lastVideoConfig: ByteArray? = null
        private set

    val port get() = server?.localPort ?: 0

    fun start(port: Int) {
        stop()
        val ss = ServerSocket()
        ss.reuseAddress = true
        ss.bind(InetSocketAddress(port))
        server = ss
        running = true
        acceptThread = Thread({
            while (running) {
                val s = try { ss.accept() } catch (e: IOException) { break }
                Thread({ handle(s) }, "mc-http").start()
            }
        }, "mc-accept").also { it.start() }
        Log.i(TAG, "listening on $port")
    }

    fun stop() {
        running = false
        try { server?.close() } catch (_: Exception) {}
        server = null
        streamClients.forEach { it.close() }
        mjpegClients.forEach { it.close() }
    }

    fun broadcastVideo(type: Int, pkt: ByteArray, key: Boolean) {
        if (type == PKT_VIDEO_CONFIG) lastVideoConfig = pkt
        for (c in streamClients) c.enqueueVideo(pkt, type, key)
    }

    fun broadcastAudio(pcm: ByteArray, ptsUs: Long) {
        if (streamClients.isEmpty()) return
        val pkt = packet(PKT_AUDIO, pcm, ptsUs, false)
        for (c in streamClients) if (c.wantsAudio) c.enqueueOther(pkt, droppable = true)
    }

    fun broadcastStatus(json: String) {
        if (streamClients.isEmpty()) return
        val pkt = packet(PKT_STATUS, json.toByteArray(), 0, false)
        for (c in streamClients) c.enqueueOther(pkt, droppable = false)
    }

    fun broadcastJpeg(jpeg: ByteArray) {
        for (c in mjpegClients) c.offer(jpeg)
        for (w in jpegWaiters) w.offer(jpeg)
    }

    private fun handle(sock: Socket) {
        try {
            sock.tcpNoDelay = true
            sock.soTimeout = 15000
            val input = sock.getInputStream()
            val requestLine = readLine(input) ?: return sock.close()
            while (true) {
                val l = readLine(input) ?: break
                if (l.isEmpty()) break
            }
            val parts = requestLine.split(" ")
            if (parts.size < 2) return sock.close()
            val target = parts[1]
            val path = target.substringBefore('?')
            val query = parseQuery(target.substringAfter('?', ""))
            val out = sock.getOutputStream()
            when (path) {
                "/", "/index.html" -> sendAsset(out, "index.html", "text/html; charset=utf-8")
                "/api/status" -> sendJson(out, core.statusJson())
                "/api/set" -> {
                    // здесь "audio" включает микрофон телефона, а в /stream это флаг конкретного клиента
                    val q = if ("audio" in query && "mic" !in query) query + ("mic" to query.getValue("audio")) else query
                    core.applyParams(q)
                    sendJson(out, core.statusJson())
                }
                "/video", "/mjpeg", "/mjpegfeed" -> { serveMjpeg(sock); return }
                "/shot.jpg", "/jpeg" -> serveShot(out)
                "/stream" -> { serveStream(sock, query); return }
                else -> sendText(out, 404, "Not found")
            }
            sock.close()
        } catch (e: Exception) {
            try { sock.close() } catch (_: Exception) {}
        }
    }

    private fun serveStream(sock: Socket, query: Map<String, String>) {
        sock.soTimeout = 0
        sock.sendBufferSize = 512 * 1024
        val wantsAudio = query["audio"] != "0"
        core.applyParams(query)
        val out = BufferedOutputStream(sock.getOutputStream(), 64 * 1024)
        out.write("HTTP/1.1 200 OK\r\nContent-Type: application/x-magercam\r\nCache-Control: no-cache\r\n\r\n".toByteArray())
        out.flush()
        val client = StreamClient(sock, out, wantsAudio)
        val ac = ByteBuffer.allocate(5).putInt(AudioCapture.RATE).put(AudioCapture.CHANNELS.toByte()).array()
        client.enqueueOther(packet(PKT_AUDIO_CONFIG, ac, 0, false), droppable = false)
        client.enqueueOther(packet(PKT_STATUS, core.statusJson().toByteArray(), 0, false), droppable = false)
        lastVideoConfig?.let { client.enqueueVideo(it, PKT_VIDEO_CONFIG, true) }
        streamClients += client
        Log.i(TAG, "stream client ${sock.inetAddress.hostAddress} audio=$wantsAudio")
        core.onClientsChanged()
        core.requestKeyFrame()
        // клиент после запроса ничего не шлёт, так что чтением я только ловлю момент отключения
        Thread({
            try { while (sock.getInputStream().read() >= 0) {} } catch (_: Exception) {}
            client.close()
        }, "mc-stream-rd").start()
        client.runWriter { core.requestKeyFrame() }
        streamClients -= client
        Log.i(TAG, "stream client gone")
        core.onClientsChanged()
    }

    private fun serveMjpeg(sock: Socket) {
        sock.soTimeout = 0
        val out = BufferedOutputStream(sock.getOutputStream(), 64 * 1024)
        out.write(("HTTP/1.0 200 OK\r\nConnection: close\r\nCache-Control: no-cache, no-store\r\n" +
            "Pragma: no-cache\r\nAccess-Control-Allow-Origin: *\r\n" +
            "Content-Type: multipart/x-mixed-replace; boundary=$BOUNDARY\r\n\r\n").toByteArray())
        out.flush()
        val client = MjpegClient(sock, out)
        mjpegClients += client
        core.onClientsChanged()
        Thread({
            try { while (sock.getInputStream().read() >= 0) {} } catch (_: Exception) {}
            client.close()
        }, "mc-mjpeg-rd").start()
        client.run()
        mjpegClients -= client
        core.onClientsChanged()
    }

    private fun serveShot(out: OutputStream) {
        val w = JpegWaiter()
        jpegWaiters += w
        core.onClientsChanged()
        val jpeg = w.await(4000)
        jpegWaiters -= w
        core.onClientsChanged()
        if (jpeg == null) return sendText(out, 503, "Camera not ready")
        out.write(("HTTP/1.0 200 OK\r\nContent-Type: image/jpeg\r\nContent-Length: ${jpeg.size}\r\n" +
            "Access-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\n\r\n").toByteArray())
        out.write(jpeg)
        out.flush()
    }

    val wantsJpeg get() = mjpegClients.isNotEmpty() || jpegWaiters.isNotEmpty()

    private fun sendAsset(out: OutputStream, name: String, type: String) {
        val bytes = core.context.assets.open(name).use { it.readBytes() }
        out.write("HTTP/1.0 200 OK\r\nContent-Type: $type\r\nContent-Length: ${bytes.size}\r\nCache-Control: no-cache\r\n\r\n".toByteArray())
        out.write(bytes)
        out.flush()
    }

    private fun sendJson(out: OutputStream, json: String) {
        val b = json.toByteArray()
        out.write(("HTTP/1.0 200 OK\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: ${b.size}\r\n" +
            "Access-Control-Allow-Origin: *\r\nCache-Control: no-cache\r\n\r\n").toByteArray())
        out.write(b)
        out.flush()
    }

    private fun sendText(out: OutputStream, code: Int, text: String) {
        val b = text.toByteArray()
        val reason = if (code == 404) "Not Found" else if (code == 503) "Service Unavailable" else "Error"
        out.write("HTTP/1.0 $code $reason\r\nContent-Type: text/plain\r\nContent-Length: ${b.size}\r\n\r\n".toByteArray())
        out.write(b)
        out.flush()
    }

    private fun readLine(input: InputStream): String? {
        val sb = StringBuilder()
        while (sb.length < 8192) {
            val c = input.read()
            if (c < 0) return if (sb.isEmpty()) null else sb.toString()
            if (c == '\n'.code) return sb.toString().trimEnd('\r')
            sb.append(c.toChar())
        }
        return sb.toString()
    }

    private fun parseQuery(q: String): Map<String, String> {
        if (q.isEmpty()) return emptyMap()
        return q.split('&').filter { it.isNotEmpty() }.associate {
            val k = it.substringBefore('=')
            val v = it.substringAfter('=', "")
            URLDecoder.decode(k, "UTF-8") to URLDecoder.decode(v, "UTF-8")
        }
    }

    class StreamClient(private val sock: Socket, private val out: OutputStream, val wantsAudio: Boolean) {
        private val queue = LinkedBlockingDeque<ByteArray>()
        private val queued = AtomicLong(0)
        @Volatile private var closed = false
        @Volatile private var waitingKey = true
        @Volatile private var hasConfig = false
        @Volatile private var keyRequested = false

        fun enqueueVideo(pkt: ByteArray, type: Int, key: Boolean) {
            if (closed) return
            if (type == PKT_VIDEO_CONFIG) {
                hasConfig = true
                waitingKey = true
                push(pkt)
                return
            }
            if (!hasConfig) return
            if (waitingKey) {
                if (!key) return
                waitingKey = false
            }
            if (queued.get() > MAX_QUEUE_BYTES) {
                // сеть не успевает: выкидываю кадры до следующего ключевого, чтобы задержка не росла
                waitingKey = true
                keyRequested = true
                return
            }
            push(pkt)
        }

        fun enqueueOther(pkt: ByteArray, droppable: Boolean) {
            if (closed) return
            if (droppable && queued.get() > MAX_QUEUE_BYTES * 2) return
            push(pkt)
        }

        private fun push(pkt: ByteArray) {
            queued.addAndGet(pkt.size.toLong())
            queue.offer(pkt)
        }

        fun runWriter(onKeyNeeded: () -> Unit) {
            try {
                while (!closed) {
                    if (keyRequested) { keyRequested = false; onKeyNeeded() }
                    val p = queue.poll(1000, TimeUnit.MILLISECONDS)
                    if (p == null) {
                        out.write(packet(PKT_PING, ByteArray(0), 0, false))
                        out.flush()
                        continue
                    }
                    queued.addAndGet(-p.size.toLong())
                    out.write(p)
                    if (queue.isEmpty()) out.flush()
                }
            } catch (_: Exception) {
            } finally {
                close()
            }
        }

        fun close() {
            closed = true
            queue.offer(ByteArray(0))
            try { sock.close() } catch (_: Exception) {}
        }
    }

    class MjpegClient(private val sock: Socket, private val out: OutputStream) {
        private val lock = Object()
        private var frame: ByteArray? = null
        @Volatile private var closed = false

        fun offer(jpeg: ByteArray) = synchronized(lock) { frame = jpeg; lock.notifyAll() }

        fun run() {
            try {
                while (!closed) {
                    val f: ByteArray = synchronized(lock) {
                        while (frame == null && !closed) lock.wait(1000)
                        val x = frame; frame = null; x
                    } ?: continue
                    out.write("--$BOUNDARY\r\nContent-Type: image/jpeg\r\nContent-Length: ${f.size}\r\n\r\n".toByteArray())
                    out.write(f)
                    out.write("\r\n".toByteArray())
                    out.flush()
                }
            } catch (_: Exception) {
            } finally {
                close()
            }
        }

        fun close() {
            closed = true
            synchronized(lock) { lock.notifyAll() }
            try { sock.close() } catch (_: Exception) {}
        }
    }

    class JpegWaiter {
        private val lock = Object()
        private var frame: ByteArray? = null
        fun offer(jpeg: ByteArray) = synchronized(lock) { if (frame == null) { frame = jpeg; lock.notifyAll() } }
        fun await(ms: Long): ByteArray? = synchronized(lock) {
            val end = System.currentTimeMillis() + ms
            while (frame == null) {
                val left = end - System.currentTimeMillis()
                if (left <= 0) break
                lock.wait(left)
            }
            frame
        }
    }

    companion object {
        private const val TAG = "MC.Server"
        const val PKT_VIDEO_CONFIG = 1
        const val PKT_VIDEO_FRAME = 2
        const val PKT_AUDIO_CONFIG = 3
        const val PKT_AUDIO = 4
        const val PKT_STATUS = 5
        const val PKT_PING = 6
        private const val BOUNDARY = "magercamframe"
        private const val MAX_QUEUE_BYTES = 1_500_000L

        fun writeHeader(dst: ByteArray, type: Int, len: Int, ptsUs: Long, key: Boolean) {
            ByteBuffer.wrap(dst, 0, 16)
                .put('M'.code.toByte()).put('C'.code.toByte()).put(type.toByte()).put(if (key) 1 else 0)
                .putInt(len).putLong(ptsUs)
        }

        fun packet(type: Int, payload: ByteArray, ptsUs: Long, key: Boolean): ByteArray {
            val pkt = ByteArray(16 + payload.size)
            writeHeader(pkt, type, payload.size, ptsUs, key)
            System.arraycopy(payload, 0, pkt, 16, payload.size)
            return pkt
        }
    }
}
