package com.magercam.app

import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test
import java.util.Random
import kotlin.math.abs
import kotlin.math.max

/**
 * Синтетические треки лиц на сенсоре 4000x3000 (4:3), выход 16:9, как у основной камеры телефона.
 * Лицо человека в метре от камеры занимает около 11% ширины кадра.
 */
class FramingControllerTest {
    private val land = FramingGeometry(4000, 3000, 16f / 9, 0, 1f, 8f)
    private val portrait = land.copy(orient = 90)

    private class Frame(val t: Double, val crop: Box?, val face: Box?)

    private fun face(cx: Double, cy: Double, size: Double) =
        Box((cx - size / 2).toFloat(), (cy - size * 0.55).toFloat(), (cx + size / 2).toFloat(), (cy + size * 0.55).toFloat())

    /** Гоняет контроллер по кадрам с частотой [fps]; [noise] — шум детектора в долях размера лица. */
    private fun run(
        fc: FramingController, g: FramingGeometry, fps: Int, from: Double, to: Double,
        noise: Double = 0.0, seed: Long = 1, enabled: (Double) -> Boolean = { true },
        faces: (Double) -> List<Box>,
    ): List<Frame> {
        val rnd = Random(seed)
        val out = ArrayList<Frame>()
        var i = 0
        while (true) {
            val t = from + i / fps.toDouble()
            if (t > to + 1e-9) break
            val fs = faces(t).map { f ->
                if (noise == 0.0) f else {
                    val s = f.w
                    val dx = (rnd.nextGaussian() * noise * s).toFloat()
                    val dy = (rnd.nextGaussian() * noise * s).toFloat()
                    val k = (1 + rnd.nextGaussian() * noise * 2.5).toFloat()
                    Box(f.cx + dx - f.w * k / 2, f.cy + dy - f.h * k / 2, f.cx + dx + f.w * k / 2, f.cy + dy + f.h * k / 2)
                }
            }
            out += Frame(t, fc.update(t, enabled(t), fs, g), fs.firstOrNull())
            i++
        }
        return out
    }

    private fun maxShift(frames: List<Frame>, ref: Box): Double = frames.maxOf { f ->
        val c = f.crop!!
        maxOf(abs(c.l - ref.l), abs(c.t - ref.t), abs(c.r - ref.r), abs(c.b - ref.b)).toDouble()
    }

    private fun assertInside(c: Box, g: FramingGeometry) {
        assertTrue("crop outside array: $c", c.l >= -0.01f && c.t >= -0.01f && c.r <= g.arrayW + 0.01f && c.b <= g.arrayH + 0.01f)
        assertEquals("aspect", g.aspect.toDouble(), (c.w / c.h).toDouble(), 1e-3)
    }

    @Test
    fun stillFaceWithNoiseDoesNotMoveFrame() {
        val fc = FramingController()
        val frames = run(fc, land, 60, 0.0, 18.0, noise = 0.03) { listOf(face(2300.0, 1300.0, 440.0)) }
        val ref = frames.first { it.t >= 8.0 }.crop!!
        val window = frames.filter { it.t >= 8.0 }
        val shift = maxShift(window, ref)
        val rects = window.map { FramingController.toPixels(it.crop!!, land).toList() }.distinct().size
        println("still face, 3%% detector noise: max crop shift over 10 s = %.2f px of 4000, distinct crop rects sent = %d".format(shift, rects))
        assertTrue("frame moved $shift px", shift < 4.0)
        assertTrue("too many crop updates: $rects", rects <= 4)
        // кадр реально наехал на лицо, а не остался общим планом
        assertTrue("not framed: ${ref.w}", ref.w < 3300)
    }

    @Test
    fun movingFaceIsFollowedSmoothlyWithoutOvershoot() {
        val fc = FramingController()
        // 2 с стоит, потом за 2 с проходит 1400 px (35% ширины сенсора) и останавливается
        fun pos(t: Double) = when {
            t < 2 -> 1300.0
            t < 4 -> 1300.0 + (t - 2) / 2 * 1400
            else -> 2700.0
        }
        val frames = run(fc, land, 60, 0.0, 10.0) { t -> listOf(face(pos(t), 1400.0, 300.0)) }
        val xs = frames.map { it.crop!!.cx.toDouble() }
        var maxStep = 0.0; var maxAcc = 0.0
        for (i in 2 until xs.size) {
            maxStep = max(maxStep, abs(xs[i] - xs[i - 1]))
            maxAcc = max(maxAcc, abs(xs[i] - 2 * xs[i - 1] + xs[i - 2]))
        }
        val final = xs.last()
        val peak = xs.filterIndexed { i, _ -> frames[i].t > 2.0 }.max()
        val endCrop = frames.last().crop!!
        println("moving face: max step %.1f px/frame, max accel %.2f px/frame^2, final cx %.0f, peak cx %.0f, face cx 2700"
            .format(maxStep, maxAcc, final, peak))
        assertTrue("jerky: $maxStep", maxStep < 25.0)
        assertTrue("jerky accel: $maxAcc", maxAcc < 2.0)
        assertTrue("overshoot ${peak - final}", peak - final < 2.0)
        assertTrue("face not in frame", abs(2700 - endCrop.cx) < endCrop.w * FramingController.DEAD_H)
    }

    @Test
    fun sizeChangeZoomsSlowlyAndOnlyWhenSustained() {
        // короткий всплеск размера на 0.4 с — зум не трогаю
        val fc = FramingController()
        run(fc, land, 60, 0.0, 8.0) { listOf(face(2000.0, 1300.0, 440.0)) }
        val before = fc.update(8.0, true, listOf(face(2000.0, 1300.0, 440.0)), land)!!
        val blip = run(fc, land, 60, 8.0 + 1 / 60.0, 12.0) { t -> listOf(face(2000.0, 1300.0, if (t < 8.4) 570.0 else 440.0)) }
        val blipShift = maxShift(blip, before)
        println("size blip x1.3 for 0.4 s: max crop change %.2f px".format(blipShift))
        assertTrue("zoom pumped on blip: $blipShift", blipShift < 4.0)

        // человек придвинулся: лицо стало в 1.25 раза крупнее и таким осталось
        val grow = run(fc, land, 60, 12.0 + 1 / 60.0, 20.0) { t ->
            val k = if (t < 12.3) 1 + (t - 12) / 0.3 * 0.25 else 1.25
            listOf(face(2000.0, 1300.0, 440.0 * k))
        }
        val ws = grow.map { it.crop!!.w.toDouble() }
        val firstChange = grow.first { abs(it.crop!!.w - before.w) > 2 }.t - 12.0
        var maxRate = 0.0
        for (i in 1 until ws.size) maxRate = max(maxRate, abs(ws[i] - ws[i - 1]) * 60 / ws[i - 1])
        println("face x1.25: zoom starts after %.2f s, max zoom speed %.1f%%/s, width %.0f -> %.0f (x%.2f)"
            .format(firstChange, maxRate * 100, before.w, ws.last(), ws.last() / before.w))
        assertTrue("zoom reacted too fast: $firstChange", firstChange > 0.7)
        assertTrue("zoom too fast: $maxRate", maxRate < 0.35)
        assertEquals(1.25, ws.last() / before.w, 0.05)
    }

    @Test
    fun shortLossHoldsFrame() {
        val fc = FramingController()
        val f = face(2300.0, 1300.0, 440.0)
        run(fc, land, 60, 0.0, 8.0) { listOf(f) }
        val ref = fc.update(8.0, true, listOf(f), land)!!
        val lost = run(fc, land, 60, 8.0 + 1 / 60.0, 12.0) { t -> if (t < 8.5) emptyList() else listOf(f) }
        val shift = maxShift(lost, ref)
        println("face lost 0.5 s: max crop shift %.2f px".format(shift))
        assertTrue("snapped on short loss: $shift", shift < 2.0)
        assertTrue(fc.faces == 1)
    }

    @Test
    fun longLossHoldsThenEasesOutToUserView() {
        val fc = FramingController()
        val f = face(2300.0, 1300.0, 440.0)
        run(fc, land, 60, 0.0, 8.0) { listOf(f) }
        val ref = fc.update(8.0, true, listOf(f), land)!!
        val lost = run(fc, land, 60, 8.0 + 1 / 60.0, 14.0) { emptyList() }
        val held = maxShift(lost.filter { it.t < 9.4 }, ref)
        val at11 = lost.first { it.t >= 11.0 }.crop!!
        val end = lost.last().crop!!
        var maxStep = 0.0
        for (i in 1 until lost.size) maxStep = max(maxStep, abs(lost[i].crop!!.w - lost[i - 1].crop!!.w).toDouble())
        println("face lost for good: shift in first 1.4 s %.2f px, width %.0f -> %.0f at +3 s -> %.0f at +6 s, max step %.1f px/frame"
            .format(held, ref.w, at11.w, end.w, maxStep))
        assertTrue("did not hold: $held", held < 2.0)
        assertTrue("not easing out", at11.w > ref.w + 100)
        assertTrue("jumped: $maxStep", maxStep < 40.0)
        assertEquals("back to user view", 4000.0, end.w.toDouble(), 30.0)
        assertEquals(0, fc.faces)
    }

    @Test
    fun sameMotionAt30And60Fps() {
        fun track(t: Double) = listOf(face(1200.0 + 1400 * ((t - 1).coerceIn(0.0, 2.0) / 2), 1300.0 + 200 * ((t - 3).coerceIn(0.0, 1.0)),
            440.0 * (1 + 0.3 * ((t - 4).coerceIn(0.0, 0.5) / 0.5))))
        val a = run(FramingController(), land, 30, 0.0, 10.0) { track(it) }
        val b = run(FramingController(), land, 60, 0.0, 10.0) { track(it) }
        var diff = 0.0
        for (fa in a) {
            val fb = b.minBy { abs(it.t - fa.t) }
            val ca = fa.crop!!; val cb = fb.crop!!
            diff = maxOf(diff, abs(ca.cx - cb.cx).toDouble(), abs(ca.cy - cb.cy).toDouble(), abs(ca.w - cb.w).toDouble())
        }
        println("30 vs 60 fps: max difference in crop over 10 s = %.1f px (%.2f%% of width)".format(diff, diff / 40))
        assertTrue("frame-rate dependent: $diff", diff < 40.0)
    }

    @Test
    fun portraitPutsFaceAtUpperThird() {
        for ((g, name) in listOf(land to "landscape", portrait to "portrait 90", land.copy(orient = 270) to "portrait 270",
            land.copy(orient = 180) to "upside down")) {
            val fc = FramingController()
            val f = face(2000.0, 1500.0, 400.0)
            val c = run(fc, g, 60, 0.0, 8.0) { listOf(f) }.last().crop!!
            assertInside(c, g)
            // где лицо в ровном кадре: доля от верха и доля высоты кадра
            val (fromTop, share) = when (g.orient) {
                90 -> (f.cx - c.l) / c.w to f.w / c.w
                270 -> (c.r - f.cx) / c.w to f.w / c.w
                180 -> (c.b - f.cy) / c.h to f.h / c.h
                else -> (f.cy - c.t) / c.h to f.h / c.h
            }
            println("%s: face centre at %.2f from top, face height %.0f%% of frame, crop %.0fx%.0f".format(name, fromTop, share * 100, c.w, c.h))
            assertEquals(name, 1.0 / 3, fromTop.toDouble(), 0.03)
            assertTrue(name, share in 0.15f..0.32f)
        }
    }

    @Test
    fun disableReturnsExactlyToUserZoomAndReenableDoesNotJump() {
        val g = land.copy(baseZoom = 1.5f)
        val fc = FramingController()
        val f = face(2600.0, 1100.0, 380.0)
        run(fc, g, 60, 0.0, 8.0) { listOf(f) }
        val (center, homeW) = FramingController.home(g)
        val homeBox = Box((center.first - homeW / 2).toFloat(), (center.second - homeW / g.aspect / 2).toFloat(),
            (center.first + homeW / 2).toFloat(), (center.second + homeW / g.aspect / 2).toFloat())
        // как в движке: запрос пересобирают сразу при выключении, и crop должен остаться текущим, без скачка
        val atOff = fc.crop(false, g)!!
        val zoomedIn = fc.update(8.0, true, listOf(f), g)
        assertNotNull(zoomedIn)
        val rel = run(fc, g, 60, 8.0 + 1 / 60.0, 11.0, enabled = { false }) { listOf(f) }
        val lastCrop = rel.last { it.crop != null }
        val offAt = rel.first { it.crop == null }.t - 8.0
        println("disable: crop %.0fx%.0f at (%.0f,%.0f) -> user view %.0fx%.0f in %.2f s, last crop %s"
            .format(atOff.w, atOff.h, atOff.cx, atOff.cy, homeW, homeW / g.aspect, offAt, lastCrop.crop))
        assertTrue("release too slow: $offAt", offAt < 1.6)
        assertTrue(rel.filter { it.t - 8.0 > offAt }.all { it.crop == null })
        // последний crop перед передачей обычному зуму отличается от вида пользователя не больше чем на шаг выравнивания
        val hp = FramingController.toPixels(homeBox, g); val lp = FramingController.toPixels(lastCrop.crop!!, g)
        assertTrue("handover jump: ${hp.toList()} vs ${lp.toList()}", hp.indices.all { abs(hp[it] - lp[it]) <= 2 })
        assertEquals(FramingController.Mode.OFF, fc.mode)

        // включаю снова: первый crop — ровно вид пользователя
        val again = fc.crop(true, g)!!
        assertArrayEquals(FramingController.toPixels(homeBox, g), FramingController.toPixels(again, g))
        val first = fc.update(11.1, true, listOf(f), g)!!
        assertTrue("jump on enable", abs(first.w - homeBox.w) < 1 && abs(first.cx - homeBox.cx) < 1)
    }

    @Test
    fun cropStaysInsideArrayAndZoomLimits() {
        val rnd = Random(7)
        val g = land.copy(maxZoom = 2f)
        val fc = FramingController()
        var cx = 2000.0; var cy = 1500.0; var s = 400.0
        val frames = run(fc, g, 60, 0.0, 60.0) { t ->
            cx = (cx + rnd.nextGaussian() * 60).coerceIn(-200.0, 4200.0)
            cy = (cy + rnd.nextGaussian() * 60).coerceIn(-200.0, 3200.0)
            s = (s * (1 + rnd.nextGaussian() * 0.03)).coerceIn(60.0, 2500.0)
            if ((t * 2).toInt() % 7 == 3) emptyList() else listOf(face(cx, cy, s), face(4000 - cx, cy, s * 0.8))
        }
        val halMin = FramingController.halMinWidth(g)
        for (f in frames) {
            val c = f.crop!!
            assertInside(c, g)
            assertTrue("zoom beyond limit: ${c.w}", c.w >= halMin - 0.5)
            val p = FramingController.toPixels(c, g)
            assertTrue(p.all { it % 2 == 0 } && p[0] >= 0 && p[1] >= 0 && p[2] <= g.arrayW && p[3] <= g.arrayH)
            assertTrue(p[2] - p[0] >= halMin - 2)
        }
        println("random walk 60 s: min crop width %.0f (HAL limit %.0f), max %.0f".format(frames.minOf { it.crop!!.w }, halMin, frames.maxOf { it.crop!!.w }))
    }

    @Test
    fun twoFacesBothInFrameAndFalsePositiveIgnored() {
        val fc = FramingController()
        val a = face(1500.0, 1400.0, 380.0)
        val b = face(2700.0, 1450.0, 360.0)
        run(fc, land, 60, 0.0, 8.0) { listOf(a, b) }
        val c = fc.update(8.0, true, listOf(a, b), land)!!
        assertTrue("faces cut: $c", c.l < a.l && c.r > b.r && c.t < a.t && c.b > b.b)
        assertEquals(2, fc.faces)
        // ложное «лицо» на один кадр в углу
        val ghost = run(fc, land, 60, 8.0 + 1 / 60.0, 11.0) { t ->
            if (abs(t - 8.5) < 0.01) listOf(a, b, face(200.0, 200.0, 500.0)) else listOf(a, b)
        }
        val shift = maxShift(ghost, c)
        println("two faces: crop %.0fx%.0f holds both; one-frame false face moved crop by %.2f px".format(c.w, c.h, shift))
        assertTrue("ghost moved frame: $shift", shift < 2.0)
    }

    @Test
    fun centerOnlyCameraZoomsWithoutCuttingFace() {
        val g = land.copy(centerOnly = true)
        val fc = FramingController()
        val f = face(2700.0, 1700.0, 400.0)
        val frames = run(fc, g, 60, 0.0, 8.0) { listOf(f) }
        for (fr in frames) {
            assertEquals(2000.0, fr.crop!!.cx.toDouble(), 0.5)
            assertEquals(1500.0, fr.crop.cy.toDouble(), 0.5)
        }
        val c = frames.last().crop!!
        println("center-only camera: crop %.0fx%.0f centred, face %s inside".format(c.w, c.h, f))
        assertTrue(c.w < 4000)
        assertTrue("face cut", c.l < f.l && c.r > f.r && c.t < f.t && c.b > f.b)
    }

    @Test
    fun userZoomWithoutFaceFollowsQuickly() {
        val fc = FramingController()
        run(fc, land, 60, 0.0, 3.0) { emptyList() }
        val g2 = land.copy(baseZoom = 2f)
        val frames = run(fc, g2, 60, 3.0 + 1 / 60.0, 6.0) { emptyList() }
        val settled = frames.first { abs(it.crop!!.w - 2000) < 20 }.t - 3.0
        println("user zoom 1x -> 2x with framing on and no face: within 1%% in %.2f s".format(settled))
        assertTrue("user zoom too laggy: $settled", settled < 1.3)
        assertEquals(2000.0, frames.last().crop!!.w.toDouble(), 2.0)
    }

    @Test
    fun inactiveWhenDisabledFromStart() {
        val fc = FramingController()
        assertNull(fc.crop(false, land))
        assertNull(fc.update(0.0, false, listOf(face(2000.0, 1500.0, 400.0)), land))
        assertEquals(FramingController.Mode.OFF, fc.mode)
    }
}
