package com.openautolink.app.video

import kotlin.math.abs
import kotlin.math.roundToInt

/**
 * Computes AA `width_margin` / `height_margin` so the inner content rect of
 * the codec frame matches the panel's aspect ratio.
 *
 * The car renderer then scales the codec frame uniformly so the inner rect
 * lands on the panel and the margin pixels overflow off-screen (clipped).
 * Result: square pixels on any panel aspect, regardless of which AA codec
 * resolution the phone picks (only 16:9 landscape and 9:16 portrait variants
 * are exposed by the AA enum, so margins are needed for any other AR).
 *
 * Mirrors the same formula in `jni_session.cpp::autoMargins` so that what
 * we tell the phone in the SDR matches what the renderer expects on the
 * decoded frame.
 */
object MarginAutoCalc {

    /**
     * @return Pair(widthMargin, heightMargin) in codec pixels. Both 0 when
     *         codec aspect already matches panel aspect.
     */
    fun compute(codecW: Int, codecH: Int, panelW: Int, panelH: Int): Pair<Int, Int> {
        if (codecW <= 0 || codecH <= 0 || panelW <= 0 || panelH <= 0) return 0 to 0
        val codecAR = codecW.toDouble() / codecH
        val panelAR = panelW.toDouble() / panelH
        // Within 0.5% — treat as matching, no margin.
        if (abs(codecAR - panelAR) / panelAR < 0.005) return 0 to 0
        return if (codecAR > panelAR) {
            // Codec wider than panel → trim left/right.
            val innerW = (codecH * panelAR).roundToInt()
            (codecW - innerW).coerceAtLeast(0) to 0
        } else {
            // Codec taller than panel → trim top/bottom.
            val innerH = (codecW / panelAR).roundToInt()
            0 to (codecH - innerH).coerceAtLeast(0)
        }
    }
}
