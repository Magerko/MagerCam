package com.magercam.app

import kotlin.math.abs
import kotlin.math.exp
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min
import kotlin.math.roundToInt

/** Прямоугольник в координатах массива сенсора, без зависимостей от Android, чтобы логику можно было гонять в тестах. */
data class Box(val l: Float, val t: Float, val r: Float, val b: Float) {
    val w get() = r - l
    val h get() = b - t
    val cx get() = (l + r) / 2
    val cy get() = (t + b) / 2
}

/**
 * Всё, что автокадрированию нужно знать о кадре.
 * [aspect] — ширина к высоте выходного потока в ориентации сенсора, [orient] — поворот по часовой,
 * после которого кадр встаёт ровно (как "orient" в статусе), [baseZoom] — зум пользователя,
 * [maxZoom] — SCALER_AVAILABLE_MAX_DIGITAL_ZOOM, [centerOnly] — камера не умеет crop region не по центру.
 */
data class FramingGeometry(
    val arrayW: Int,
    val arrayH: Int,
    val aspect: Float,
    val orient: Int,
    val baseZoom: Float,
    val maxZoom: Float,
    val centerOnly: Boolean = false,
)

/**
 * Автокадрирование в духе Center Stage. Лица и crop region живут в одних координатах массива сенсора,
 * а «верх» и пропорции считаю в ровном кадре, каким его видит зритель после поворота на [FramingGeometry.orient].
 *
 * Как устроено: лица сглаживаю и склеиваю в треки, из них считаю желаемый кадр (голова с плечами, лицо на трети сверху),
 * цель меняю только когда лицо ушло из мёртвой зоны или размер стабильно поменялся, а сам кадр тянется к цели
 * критически демпфированной пружиной по времени, поэтому на 30 и 60 к/с движение одинаковое.
 * Время — в секундах, любая монотонная шкала.
 */
class FramingController {
    enum class Mode { OFF, ON, RELEASING }

    var mode = Mode.OFF; private set
    val isActive get() = mode != Mode.OFF
    /** Сколько лиц сейчас подтверждено. */
    var faces = 0; private set

    private class Track(var box: Box, val born: Double, var seen: Double, var hits: Int = 1)
    private val tracks = ArrayList<Track>()

    // кадр: центр и ширина по оси x сенсора, высота всегда ширина / aspect
    private var x = 0.0; private var y = 0.0; private var w = 0.0
    // положение кадра храню долей свободного хода (0 — у левого/верхнего края, 1 — у правого/нижнего):
    // тогда при зуме кадр не упирается в край и не ломает плавность движения
    private var u = 0.5; private var q = 0.5
    private var vu = 0.0; private var vq = 0.0; private var vw = 0.0
    private var tx = 0.0; private var ty = 0.0; private var tw = 0.0
    // цель после короткого сглаживания: так у пружины нет рывка ускорения, когда цель переставили
    private var fx = 0.0; private var fy = 0.0; private var fw = 0.0
    private var lastT = Double.NaN

    // сглаженный объединённый бокс лиц
    private var hasMeas = false
    private var mx = 0.0; private var my = 0.0; private var mw = 0.0; private var mh = 0.0

    private var subject = false
    private var lastSeen = Double.NEGATIVE_INFINITY
    private var lostNearEdge = false
    private var posTracking = false
    private var posRefX = 0.0; private var posRefY = 0.0; private var posRefT = 0.0
    private var zoomTracking = false
    private var zoomOutsideSince = Double.NaN
    private var zoomRef = 0.0; private var zoomRefT = 0.0
    private var releaseStart = 0.0
    private var lastHomeW = Double.NaN
    // к виду пользователя еду медленно, только когда потерял лицо; его собственный зум отрабатываю быстро
    private var easeOut = false

    fun reset() {
        mode = Mode.OFF
        faces = 0
        tracks.clear()
        hasMeas = false
        subject = false
        posTracking = false
        zoomTracking = false
        lastT = Double.NaN
        lastHomeW = Double.NaN
        easeOut = false
    }

    /**
     * Какой crop нужен прямо сейчас, без шага по времени: зовётся, когда запрос к камере пересобирают.
     * Включение начинаю с того вида, что уже на экране (зум пользователя), поэтому скачка нет.
     * null — автокадрирование не участвует, зум ведёт обычный путь.
     */
    fun crop(enabled: Boolean, g: FramingGeometry): Box? {
        transition(enabled, g, lastT)
        return if (mode == Mode.OFF) null else output(g)
    }

    /** Шаг по кадру с камеры. [faceBoxes] — уже отфильтрованные по уверенности лица в координатах массива. */
    fun update(now: Double, enabled: Boolean, faceBoxes: List<Box>, g: FramingGeometry): Box? {
        transition(enabled, g, now)
        if (mode == Mode.OFF) { lastT = now; return null }
        val dt = if (lastT.isNaN()) 0.0 else (now - lastT).coerceIn(0.0, 0.1)
        lastT = now

        val (home, homeW) = home(g)
        val wMin = minWidth(g, homeW)
        if (!lastHomeW.isNaN() && abs(homeW - lastHomeW) > 0.5) easeOut = false
        lastHomeW = homeW

        val posT: Double
        val zoomT: Double
        if (mode == Mode.RELEASING) {
            faces = 0
            tx = home.first; ty = home.second; tw = homeW
            posT = RELEASE_T; zoomT = RELEASE_T
        } else {
            track(now, faceBoxes)
            val sub = subjectBox(now)
            faces = confirmed(now).size
            if (sub != null) {
                // трек живёт ещё TRACK_KEEP после последнего обнаружения, но потерю отсчитываю от обнаружения
                lastSeen = sub.second
                follow(now, dt, sub.first, g, homeW, wMin)
                posT = FOLLOW_POS_T; zoomT = FOLLOW_ZOOM_T
            } else {
                val hold = if (lostNearEdge) HOLD_EDGE else HOLD
                if (subject && now - lastSeen < hold) {
                    // держу кадр там, где лицо пропало: детектор часто теряет его на пару кадров
                    posTracking = false; zoomTracking = false
                    posT = FOLLOW_POS_T; zoomT = FOLLOW_ZOOM_T
                } else {
                    if (subject) easeOut = true
                    subject = false
                    hasMeas = false
                    posTracking = false; zoomTracking = false
                    tx = home.first; ty = home.second; tw = homeW
                    if (easeOut) { posT = HOME_POS_T; zoomT = HOME_ZOOM_T } else { posT = RELEASE_T; zoomT = RELEASE_T }
                }
            }
        }
        // зум пользователя могли поменять на ходу: цель всегда в пределах
        tw = tw.coerceIn(wMin, homeW)
        val tc = clampCenter(tx, ty, tw, g); tx = tc.first; ty = tc.second

        spring(dt, posT, zoomT, g)

        if (mode == Mode.RELEASING) {
            // отдаю обычному зуму, как только crop в пикселях совпал с видом пользователя: дальше разницы не видно
            val hb = Box((tx - tw / 2).toFloat(), (ty - tw / g.aspect / 2).toFloat(), (tx + tw / 2).toFloat(), (ty + tw / g.aspect / 2).toFloat())
            val speed = abs(vu) * (g.arrayW - w) + abs(vq) * (g.arrayH - w / g.aspect) + abs(vw)
            val done = toPixels(output(g), g).contentEquals(toPixels(hb, g)) && speed < 20.0
            if (done || now - releaseStart > RELEASE_MAX) {
                reset()
                return null
            }
        }
        return output(g)
    }

    private fun transition(enabled: Boolean, g: FramingGeometry, now: Double) {
        when {
            enabled && mode == Mode.OFF -> {
                val (home, homeW) = home(g)
                x = home.first; y = home.second; w = homeW
                u = 0.5; q = 0.5
                vu = 0.0; vq = 0.0; vw = 0.0
                tx = x; ty = y; tw = w
                fx = x; fy = y; fw = w
                tracks.clear(); hasMeas = false; subject = false
                posTracking = false; zoomTracking = false; zoomOutsideSince = Double.NaN
                lastSeen = Double.NEGATIVE_INFINITY
                mode = Mode.ON
            }
            enabled && mode == Mode.RELEASING -> mode = Mode.ON
            !enabled && mode == Mode.ON -> {
                mode = Mode.RELEASING
                releaseStart = if (now.isNaN()) 0.0 else now
                if (now.isNaN()) lastT = Double.NaN
            }
        }
    }

    private fun follow(now: Double, dt: Double, sub: Box, g: FramingGeometry, homeW: Double, wMin: Double) {
        // мера: сглаживаю объединённый бокс; позицию быстрее, размер медленнее, он у детектора шумит сильнее
        if (!hasMeas) {
            mx = sub.cx.toDouble(); my = sub.cy.toDouble(); mw = sub.w.toDouble(); mh = sub.h.toDouble()
            hasMeas = true
        } else {
            val ap = 1 - exp(-dt / MEAS_POS_T)
            val asz = 1 - exp(-dt / MEAS_SIZE_T)
            mx += (sub.cx - mx) * ap; my += (sub.cy - my) * ap
            mw += (sub.w - mw) * asz; mh += (sub.h - mh) * asz
        }
        val acquired = !subject
        subject = true

        val wDes = desiredWidth(g, homeW, wMin)
        if (acquired) {
            tw = wDes
            zoomTracking = true; zoomRef = wDes; zoomRefT = now; zoomOutsideSince = Double.NaN
        } else {
            val dev = ln(wDes / tw)
            if (!zoomTracking) {
                if (abs(dev) > ZOOM_BAND) {
                    if (zoomOutsideSince.isNaN()) zoomOutsideSince = now
                    // лицо стало заметно крупнее кадра — отъезжаю быстрее, чтобы не резать голову
                    val need = if (dev > ZOOM_BAND_URGENT) ZOOM_SUSTAIN_URGENT else ZOOM_SUSTAIN
                    if (now - zoomOutsideSince >= need) {
                        zoomTracking = true; zoomRef = wDes; zoomRefT = now
                    }
                } else zoomOutsideSince = Double.NaN
            }
            if (zoomTracking) {
                tw = wDes
                if (abs(ln(wDes / zoomRef)) > ZOOM_SETTLE_EPS) { zoomRef = wDes; zoomRefT = now }
                else if (now - zoomRefT >= ZOOM_SETTLE_T) { zoomTracking = false; zoomOutsideSince = Double.NaN }
            }
        }

        val (dx, dy) = desiredCenter(g, tw)
        val rot = g.orient % 180 != 0
        val upW = if (rot) tw / g.aspect else tw
        val upH = if (rot) tw else tw / g.aspect
        fun errs(ax: Double, ay: Double): Pair<Double, Double> {
            val ex = ax - tx; val ey = ay - ty
            return if (rot) abs(ey) / upW to abs(ex) / upH else abs(ex) / upW to abs(ey) / upH
        }
        if (acquired) {
            tx = dx; ty = dy
            posTracking = true; posRefX = dx; posRefY = dy; posRefT = now
        } else {
            if (!posTracking) {
                val (eh, ev) = errs(dx, dy)
                if (eh > DEAD_H || ev > DEAD_V) {
                    posTracking = true; posRefX = dx; posRefY = dy; posRefT = now
                }
            }
            if (posTracking) {
                tx = dx; ty = dy
                val movH = if (rot) abs(dy - posRefY) / upW else abs(dx - posRefX) / upW
                val movV = if (rot) abs(dx - posRefX) / upH else abs(dy - posRefY) / upH
                if (movH > SETTLE_EPS || movV > SETTLE_EPS) { posRefX = dx; posRefY = dy; posRefT = now }
                else if (now - posRefT >= SETTLE_T) posTracking = false
            }
        }

        // лицо у самого края кадра: детектор часто видит только то, что внутри crop, ждать долго незачем
        val ch = w / g.aspect
        val mxE = w * EDGE_MARGIN; val myE = ch * EDGE_MARGIN
        lostNearEdge = sub.l < x - w / 2 + mxE || sub.r > x + w / 2 - mxE ||
            sub.t < y - ch / 2 + myE || sub.b > y + ch / 2 - myE
    }

    private fun desiredWidth(g: FramingGeometry, homeW: Double, wMin: Double): Double {
        val rot = g.orient % 180 != 0
        val faceUpW = if (rot) mh else mw
        val faceUpH = if (rot) mw else mh
        val upAspect = if (rot) 1.0 / g.aspect else g.aspect.toDouble()
        // высоту кадра задаёт голова, ширину — плечи; беру то, что требует кадр пошире
        val upH = max(faceUpH / FACE_HEIGHT, faceUpW * SHOULDERS / upAspect)
        var wd = if (rot) upH else upH * g.aspect
        if (g.centerOnly) {
            // кадр всегда по центру: зумлю не дальше, чем лица ещё помещаются с запасом
            val cx = g.arrayW / 2.0; val cy = g.arrayH / 2.0
            val hx = max(abs(mx - mw / 2 - cx), abs(mx + mw / 2 - cx))
            val hy = max(abs(my - mh / 2 - cy), abs(my + mh / 2 - cy))
            val k = 1 - 2 * CONTAIN_MARGIN
            wd = max(wd, max(2 * hx / k, 2 * hy * g.aspect / k))
        }
        return wd.coerceIn(wMin, homeW)
    }

    private fun desiredCenter(g: FramingGeometry, width: Double): Pair<Double, Double> {
        if (g.centerOnly) return g.arrayW / 2.0 to g.arrayH / 2.0
        val rot = g.orient % 180 != 0
        val upH = if (rot) width else width / g.aspect
        // лицо на трети сверху: центр кадра ниже центра лица на (1/2 - 1/3) высоты
        val off = upH * (0.5 - FACE_FROM_TOP)
        val (ux, uy) = up(g.orient)
        return mx - ux * off to my - uy * off
    }

    private fun spring(dt: Double, posT: Double, zoomT: Double, g: FramingGeometry) {
        if (dt <= 0) return
        // быстрые переходы (выключение, зум пользователя) веду одной пружиной, без лишней задержки фильтра
        if (posT <= RELEASE_T) {
            fx = tx; fy = ty; fw = tw
        } else {
            val a = 1 - exp(-dt / TARGET_T)
            fx += (tx - fx) * a; fy += (ty - fy) * a; fw += (tw - fw) * a
        }
        fun step(p: Double, v: Double, target: Double, t: Double): Pair<Double, Double> {
            // точное решение критически демпфированной пружины на шаге dt, поэтому не зависит от частоты кадров
            val om = 2.0 / t
            val c = p - target
            val e = exp(-om * dt)
            val tmp = (v + om * c) * dt
            return target + (c + tmp) * e to (v - om * tmp) * e
        }
        fun frac(c: Double, size: Double, span: Int): Double {
            val slack = span - size
            return if (slack < 1) 0.5 else ((c - size / 2) / slack).coerceIn(0.0, 1.0)
        }
        step(u, vu, frac(fx, fw, g.arrayW), posT).let { u = it.first; vu = it.second }
        step(q, vq, frac(fy, fw / g.aspect, g.arrayH), posT).let { q = it.first; vq = it.second }
        step(w, vw, fw, zoomT).let { w = it.first; vw = it.second }
        w = w.coerceIn(min(halMinWidth(g), fullWidth(g)), fullWidth(g))
        x = w / 2 + u.coerceIn(0.0, 1.0) * (g.arrayW - w)
        y = w / g.aspect / 2 + q.coerceIn(0.0, 1.0) * (g.arrayH - w / g.aspect)
    }

    private fun output(g: FramingGeometry): Box {
        val full = fullWidth(g)
        val wHal = halMinWidth(g)
        val ww = w.coerceIn(min(wHal, full), full)
        val (cx, cy) = clampCenter(x, y, ww, g)
        val hh = ww / g.aspect
        return Box((cx - ww / 2).toFloat(), (cy - hh / 2).toFloat(), (cx + ww / 2).toFloat(), (cy + hh / 2).toFloat())
    }

    private fun track(now: Double, faceBoxes: List<Box>) {
        val used = BooleanArray(tracks.size)
        for (f in faceBoxes) {
            if (f.w <= 0 || f.h <= 0) continue
            var best = -1; var bestScore = 0.0
            for (i in used.indices) {
                if (used[i]) continue
                val t = tracks[i]
                val iou = iou(f, t.box)
                val size = max(max(t.box.w, t.box.h), max(f.w, f.h)).toDouble()
                val dist = kotlin.math.hypot((f.cx - t.box.cx).toDouble(), (f.cy - t.box.cy).toDouble())
                val s = if (iou > 0.1) 1 + iou else if (dist < size * 0.75) 1 - dist / size else 0.0
                if (s > bestScore) { bestScore = s; best = i }
            }
            if (best >= 0) {
                used[best] = true
                tracks[best].box = f; tracks[best].seen = now; tracks[best].hits++
            } else {
                tracks += Track(f, now, now)
            }
        }
        tracks.removeAll { now - it.seen > TRACK_KEEP }
    }

    /** Лицо засчитываю, только если детектор видел его несколько раз на протяжении [CONFIRM]: одиночные ложные срабатывания отсеиваются. */
    private fun confirmed(now: Double) =
        tracks.filter { it.seen - it.born >= CONFIRM && it.hits >= CONFIRM_HITS && now - it.seen <= TRACK_KEEP }

    /** Объединение подтверждённых лиц; мелкие лица на заднем плане не считаю. */
    private fun subjectBox(now: Double): Pair<Box, Double>? {
        val c = confirmed(now)
        if (c.isEmpty()) return null
        val biggest = c.maxOf { max(it.box.w, it.box.h) }
        var l = Float.MAX_VALUE; var t = Float.MAX_VALUE; var r = -Float.MAX_VALUE; var b = -Float.MAX_VALUE
        for (tr in c) {
            if (max(tr.box.w, tr.box.h) < biggest * MIN_FACE_SHARE) continue
            l = min(l, tr.box.l); t = min(t, tr.box.t); r = max(r, tr.box.r); b = max(b, tr.box.b)
        }
        return Box(l, t, r, b) to c.maxOf { it.seen }
    }

    companion object {
        const val FACE_HEIGHT = 0.28
        const val SHOULDERS = 2.8
        const val FACE_FROM_TOP = 1.0 / 3
        const val MAX_ZOOM = 3.0
        const val DEAD_H = 0.10
        const val DEAD_V = 0.07
        const val SETTLE_EPS = 0.02
        const val SETTLE_T = 0.4
        const val ZOOM_BAND = 0.15
        const val ZOOM_BAND_URGENT = 0.30
        const val ZOOM_SUSTAIN = 0.8
        const val ZOOM_SUSTAIN_URGENT = 0.3
        const val ZOOM_SETTLE_EPS = 0.04
        const val ZOOM_SETTLE_T = 0.6
        const val MEAS_POS_T = 0.12
        const val MEAS_SIZE_T = 0.4
        const val FOLLOW_POS_T = 0.5
        const val FOLLOW_ZOOM_T = 1.0
        const val HOME_POS_T = 1.2
        const val HOME_ZOOM_T = 1.5
        const val TARGET_T = 0.2
        const val RELEASE_T = 0.3
        const val RELEASE_MAX = 1.5
        const val HOLD = 1.5
        const val HOLD_EDGE = 0.5
        const val EDGE_MARGIN = 0.06
        const val CONTAIN_MARGIN = 0.15
        const val TRACK_KEEP = 0.5
        const val CONFIRM = 0.2
        const val CONFIRM_HITS = 3
        const val MIN_FACE_SHARE = 0.4

        /** Направление «вверх» ровного кадра в координатах сенсора. */
        fun up(orient: Int): Pair<Double, Double> = when ((orient % 360 + 360) % 360) {
            90 -> -1.0 to 0.0
            180 -> 0.0 to 1.0
            270 -> 1.0 to 0.0
            else -> 0.0 to -1.0
        }

        fun fullWidth(g: FramingGeometry) = min(g.arrayW.toDouble(), g.arrayH * g.aspect.toDouble())

        /** Меньше этого камера crop region не примет: SCALER_AVAILABLE_MAX_DIGITAL_ZOOM по обеим осям. */
        fun halMinWidth(g: FramingGeometry): Double {
            val z = max(g.maxZoom.toDouble(), 1.0)
            return max(g.arrayW / z, g.arrayH * g.aspect / z)
        }

        /** Вид пользователя: кадр по центру с его зумом. Его же отдаёт обычный путь зума, поэтому переход без скачка. */
        fun home(g: FramingGeometry): Pair<Pair<Double, Double>, Double> {
            val full = fullWidth(g)
            val wh = (full / max(g.baseZoom.toDouble(), 1.0)).coerceIn(min(halMinWidth(g), full), full)
            return (g.arrayW / 2.0 to g.arrayH / 2.0) to wh
        }

        fun minWidth(g: FramingGeometry, homeW: Double) =
            max(halMinWidth(g), min(homeW, fullWidth(g) / MAX_ZOOM)).coerceAtMost(homeW)

        fun clampCenter(cx: Double, cy: Double, width: Double, g: FramingGeometry): Pair<Double, Double> {
            val hw = width / 2; val hh = width / g.aspect / 2
            val x = if (g.arrayW <= 2 * hw) g.arrayW / 2.0 else cx.coerceIn(hw, g.arrayW - hw)
            val y = if (g.arrayH <= 2 * hh) g.arrayH / 2.0 else cy.coerceIn(hh, g.arrayH - hh)
            return x to y
        }

        /**
         * Целые координаты для SCALER_CROP_REGION: чётные, внутри массива, с пропорцией выхода,
         * иначе камера сама дорежет кадр и картинка начнёт подрагивать на округлениях.
         */
        fun toPixels(b: Box, g: FramingGeometry): IntArray {
            val aw = g.arrayW - g.arrayW % 2
            val ah = g.arrayH - g.arrayH % 2
            var wI = ((b.w / 2).roundToInt() * 2).coerceIn(2, aw)
            var hI = ((wI / g.aspect / 2).roundToInt() * 2)
            if (hI > ah) { hI = ah; wI = ((hI * g.aspect / 2).roundToInt() * 2).coerceIn(2, aw) }
            hI = hI.coerceIn(2, ah)
            val l = ((b.cx - wI / 2f) / 2).roundToInt() * 2
            val t = ((b.cy - hI / 2f) / 2).roundToInt() * 2
            val lc = l.coerceIn(0, aw - wI)
            val tc = t.coerceIn(0, ah - hI)
            return intArrayOf(lc, tc, lc + wI, tc + hI)
        }

        private fun iou(a: Box, b: Box): Double {
            val iw = min(a.r, b.r) - max(a.l, b.l)
            val ih = min(a.b, b.b) - max(a.t, b.t)
            if (iw <= 0 || ih <= 0) return 0.0
            val inter = iw.toDouble() * ih
            return inter / (a.w.toDouble() * a.h + b.w.toDouble() * b.h - inter)
        }
    }
}
