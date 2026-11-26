package com.magercam.app

import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.params.ColorSpaceTransform
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.ln

/**
 * Ручной баланс белого по цветовой температуре. Соглашение как в фоторедакторах и камерах: wb_temp — температура
 * света, который должен выйти нейтральным. Под лампой накаливания 3200 K ставлю 3200 — белое становится белым;
 * значение ниже реального света даёт картинку холоднее (синее), выше — теплее. Оттенок: плюс — в пурпур
 * (убирает зелень люминесцентных ламп), минус — в зелень, как ползунок Tint в Lightroom.
 *
 * Матрицы 3×3 храню по строкам.
 */
object WhiteBalance {
    const val TEMP_MIN = 2000
    const val TEMP_MAX = 10000
    const val TEMP_DEFAULT = 5000
    const val TINT_LIMIT = 50
    /** Без калибровки сенсора считаю, что автобаланс выдаёт кадр, как если бы стоял этот баланс. */
    const val REF_KELVIN = 5000
    // ±50 единиц оттенка — ±0.025 Δuv от планковской кривой: с запасом хватает на зелёные лампы
    private const val TINT_UV = 1.0 / 2000

    private val XYZ_TO_SRGB = doubleArrayOf(
        3.2404542, -1.5371385, -0.4985314,
        -0.9692660, 1.8760108, 0.0415560,
        0.0556434, -0.2040259, 1.0572252)
    private val SRGB_TO_XYZ = doubleArrayOf(
        0.4124564, 0.3575761, 0.1804375,
        0.2126729, 0.7151522, 0.0721750,
        0.0193339, 0.1191920, 0.9503041)
    private val BRADFORD = doubleArrayOf(
        0.8951, 0.2664, -0.1614,
        -0.7502, 1.7135, 0.0367,
        0.0389, -0.0685, 1.0296)
    private val BRADFORD_INV = inv(BRADFORD)!!
    private val IDENTITY = doubleArrayOf(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)

    /** Планковская кривая в CIE 1960 uv, аппроксимация Krystek (1985), точность лучше 1e-4 на 1000..15000 K. */
    private fun planckUv(t: Double) = doubleArrayOf(
        (0.860117757 + 1.54118254e-4 * t + 1.28641212e-7 * t * t) / (1 + 8.42420235e-4 * t + 7.08145163e-7 * t * t),
        (0.317398726 + 4.22806245e-5 * t + 4.20481691e-8 * t * t) / (1 - 2.89741816e-5 * t + 1.61456053e-7 * t * t))

    /** XYZ (Y = 1) источника с температурой [kelvin]; оттенок сдвигает его по нормали к кривой, плюс — в сторону зелёного. */
    fun whiteXyz(kelvin: Int, tint: Int): DoubleArray {
        val t = kelvin.coerceIn(1000, 15000).toDouble()
        val p = planckUv(t)
        if (tint != 0) {
            val a = planckUv(t - 10); val b = planckUv(t + 10)
            var nu = b[1] - a[1]; var nv = -(b[0] - a[0])
            val len = hypot(nu, nv)
            if (nv < 0) { nu = -nu; nv = -nv }
            // источник «зеленее» на столько, сколько пурпура нужно добавить в кадр
            p[0] += nu / len * tint * TINT_UV
            p[1] += nv / len * tint * TINT_UV
        }
        val d = 2 * p[0] - 8 * p[1] + 4
        val x = 3 * p[0] / d
        val y = 2 * p[1] / d
        return doubleArrayOf(x / y, 1.0, (1 - x - y) / y)
    }

    /**
     * Сдвиг баланса уже готового кадра (линейный sRGB): адаптация Брэдфорда, при которой белое под светом [kelvin]
     * становится белым кадра, сбалансированного на [refKelvin]. Яркость белого сохраняю.
     */
    fun adaptSrgb(kelvin: Int, tint: Int, refKelvin: Int): DoubleArray {
        val src = mulv(BRADFORD, whiteXyz(kelvin, tint))
        val dst = mulv(BRADFORD, whiteXyz(refKelvin, 0))
        val d = doubleArrayOf(dst[0] / src[0], 0.0, 0.0, 0.0, dst[1] / src[1], 0.0, 0.0, 0.0, dst[2] / src[2])
        val a = mul(XYZ_TO_SRGB, mul(BRADFORD_INV, mul(d, mul(BRADFORD, SRGB_TO_XYZ))))
        val white = mulv(a, doubleArrayOf(1.0, 1.0, 1.0))
        val y = SRGB_TO_XYZ[3] * white[0] + SRGB_TO_XYZ[4] * white[1] + SRGB_TO_XYZ[5] * white[2]
        return if (y > 0) DoubleArray(9) { a[it] / y } else a
    }

    /** Матрица 3×3 для glUniformMatrix3fv (по столбцам). */
    fun toGl(m: DoubleArray) = FloatArray(9) { m[(it % 3) * 3 + it / 3].toFloat() }

    fun toDoubles(t: ColorSpaceTransform) = DoubleArray(9) { t.getElement(it % 3, it / 3).toDouble() }

    fun toTransform(m: DoubleArray): ColorSpaceTransform {
        val e = IntArray(18)
        for (i in 0 until 9) { e[i * 2] = Math.round(m[i] * 10000).toInt(); e[i * 2 + 1] = 10000 }
        return ColorSpaceTransform(e)
    }

    /**
     * Калибровка сенсора в духе DNG: ColorMatrix (XYZ -> эталонный сенсор) и CameraCalibration (эталонный ->
     * этот экземпляр) для одного-двух эталонных источников. По ней знаю, какой сигнал даст на сенсоре белое
     * под светом любой температуры, и считаю усиления каналов в абсолютных кельвинах.
     */
    class Calibration(
        private val cm1: DoubleArray, private val cc1: DoubleArray, private val t1: Double,
        private val cm2: DoubleArray?, private val cc2: DoubleArray?, private val t2: Double,
    ) {
        /** Усиления R, G, B, при которых свет [kelvin] выходит нейтральным; null, если калибровка даёт чушь. */
        fun gains(kelvin: Int, tint: Int): DoubleArray? {
            var cm = cm1; var cc = cc1
            if (cm2 != null && cc2 != null && t1 != t2) {
                // как в DNG: интерполирую по обратной температуре между двумя эталонами
                val g = ((1.0 / kelvin - 1.0 / t2) / (1.0 / t1 - 1.0 / t2)).coerceIn(0.0, 1.0)
                cm = DoubleArray(9) { g * cm1[it] + (1 - g) * cm2[it] }
                cc = DoubleArray(9) { g * cc1[it] + (1 - g) * cc2[it] }
            }
            val n = mulv(cc, mulv(cm, whiteXyz(kelvin, tint)))
            if (n.any { it <= 1e-6 }) return null
            val out = doubleArrayOf(n[1] / n[0], 1.0, n[1] / n[2])
            // усиление меньше единицы недотягивает канал до белого в светах, и они уходят в цвет
            val min = out.minOrNull()!!
            if (min < 1.0) for (i in 0 until 3) out[i] /= min
            return out
        }

        /** Ближайшая температура для усилений автобаланса и насколько они расходятся (сумма |ln| по R/G и B/G). */
        fun fit(r: Double, g: Double, b: Double): Pair<Int, Double> {
            var best = TEMP_DEFAULT; var err = Double.MAX_VALUE
            for (k in TEMP_MIN..TEMP_MAX step 50) {
                val c = gains(k, 0) ?: continue
                val e = abs(ln((c[0] / c[1]) / (r / g))) + abs(ln((c[2] / c[1]) / (b / g)))
                if (e < err) { err = e; best = k }
            }
            return best to err
        }

        /** Свет теплее — красного нужно меньше, синего больше; иначе калибровка не та, за которую себя выдаёт. */
        fun plausible(): Boolean {
            val warm = gains(2500, 0) ?: return false
            val cold = gains(7500, 0) ?: return false
            if ((warm + cold).any { it !in 0.2..16.0 }) return false
            return warm[0] / warm[1] < cold[0] / cold[1] && warm[2] / warm[1] > cold[2] / cold[1]
        }

        companion object {
            fun of(ch: CameraCharacteristics): Calibration? {
                val cm1 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM1)?.let { toDoubles(it) } ?: return null
                val t1 = ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT1)?.let { illuminantKelvin(it) } ?: return null
                if (inv(cm1) == null) return null
                val cc1 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM1)?.let { toDoubles(it) } ?: IDENTITY
                val t2 = ch.get(CameraCharacteristics.SENSOR_REFERENCE_ILLUMINANT2)?.let { illuminantKelvin(it.toInt()) }
                val cm2 = ch.get(CameraCharacteristics.SENSOR_COLOR_TRANSFORM2)?.let { toDoubles(it) }?.takeIf { inv(it) != null }
                val cc2 = ch.get(CameraCharacteristics.SENSOR_CALIBRATION_TRANSFORM2)?.let { toDoubles(it) } ?: IDENTITY
                val c = if (t2 != null && cm2 != null) Calibration(cm1, cc1, t1, cm2, cc2, t2)
                    else Calibration(cm1, cc1, t1, null, null, t1)
                return if (c.plausible()) c else null
            }

            /** Температуры эталонных источников EXIF/DNG LightSource, на которые ссылается Camera2. */
            private fun illuminantKelvin(v: Int): Double? = when (v) {
                1, 4, 9 -> 5500.0          // дневной свет, вспышка, ясно
                2, 14 -> 4150.0            // люминесцентная, холодная белая
                3 -> 2850.0                // лампа накаливания
                10 -> 6500.0               // облачно
                11 -> 7500.0               // тень
                12 -> 6430.0
                13 -> 5000.0
                15 -> 3450.0
                16 -> 2940.0
                17 -> 2856.0               // стандартный A
                18 -> 4874.0
                19 -> 6774.0
                20 -> 5503.0
                21 -> 6504.0               // D65
                22 -> 7504.0
                23 -> 5003.0
                24 -> 3200.0               // студийная лампа ISO
                else -> null
            }
        }
    }

    fun mul(a: DoubleArray, b: DoubleArray) = DoubleArray(9) { i ->
        val r = i / 3; val c = i % 3
        a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c]
    }

    private fun mulv(a: DoubleArray, v: DoubleArray) = DoubleArray(3) { r -> a[r * 3] * v[0] + a[r * 3 + 1] * v[1] + a[r * 3 + 2] * v[2] }

    private fun inv(m: DoubleArray): DoubleArray? {
        val det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) + m[2] * (m[3] * m[7] - m[4] * m[6])
        if (abs(det) < 1e-9) return null
        return doubleArrayOf(
            (m[4] * m[8] - m[5] * m[7]) / det, (m[2] * m[7] - m[1] * m[8]) / det, (m[1] * m[5] - m[2] * m[4]) / det,
            (m[5] * m[6] - m[3] * m[8]) / det, (m[0] * m[8] - m[2] * m[6]) / det, (m[2] * m[3] - m[0] * m[5]) / det,
            (m[3] * m[7] - m[4] * m[6]) / det, (m[1] * m[6] - m[0] * m[7]) / det, (m[0] * m[4] - m[1] * m[3]) / det)
    }
}
