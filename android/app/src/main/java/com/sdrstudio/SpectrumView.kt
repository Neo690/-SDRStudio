package com.sdrstudio

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.Path
import android.util.AttributeSet
import android.view.MotionEvent
import android.view.View

/**
 * Spectre + waterfall dessines en logiciel (Bitmap + Canvas).
 * Les bins viennent du coeur DSP natif : 4096 valeurs en dB.
 */
class SpectrumView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null
) : View(context, attrs) {

    var onTune: ((Double) -> Unit)? = null
    var sampleRateHz: Double = 2.4e6

    // -200 dB par defaut : si le moteur n'a pas encore rempli les bins, on affiche
    // le plancher et non un faux signal a 0 dB.
    private val bins = FloatArray(4096) { -200f }
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        strokeWidth = 1.5f
        style = Paint.Style.STROKE
    }
    private val path = Path()

    // Couleurs du waterfall (dB -> couleur).
    private val palette = IntArray(256)

    private var waterfall: Bitmap? = null
    private var scratch: Bitmap? = null
    private var waterfallCanvas: Canvas? = null
    private var scratchCanvas: Canvas? = null

    private var rowPixels = IntArray(1)
    private var rowDb = FloatArray(1)
    private var lastRowAt = 0L
    private val rowIntervalMs = 45L

    private val minDb = -110f
    private val maxDb = -15f

    init {
        buildPalette()
    }

    private fun buildPalette() {
        val stops = intArrayOf(
            Color.rgb(6, 10, 26),
            Color.rgb(12, 32, 96),
            Color.rgb(10, 92, 160),
            Color.rgb(12, 168, 168),
            Color.rgb(60, 200, 110),
            Color.rgb(230, 214, 76),
            Color.rgb(255, 150, 50),
            Color.rgb(255, 250, 235)
        )
        for (i in 0 until 256) {
            val pos = i / 255f * (stops.size - 1)
            val index = pos.toInt().coerceAtMost(stops.size - 2)
            val t = pos - index
            val a = stops[index]
            val b = stops[index + 1]
            val r = (Color.red(a) + (Color.red(b) - Color.red(a)) * t).toInt()
            val g = (Color.green(a) + (Color.green(b) - Color.green(a)) * t).toInt()
            val bl = (Color.blue(a) + (Color.blue(b) - Color.blue(a)) * t).toInt()
            palette[i] = Color.rgb(r, g, bl)
        }
    }

    private fun normalized(db: Float): Float =
        ((db - minDb) / (maxDb - minDb)).coerceIn(0f, 1f)

    /** Un pixel = le maximum des bins couverts, pour ne pas perdre un pic etroit. */
    private fun sampleRow(width: Int): FloatArray {
        if (rowDb.size != width) rowDb = FloatArray(width)
        val n = bins.size
        val half = (n / (2 * width)).coerceAtLeast(1)
        for (x in 0 until width) {
            val center = ((x.toFloat() / width - 0.5f) * n).toInt()
            var best = -200f
            for (k in -half..half) {
                val idx = ((center + k) % n + n) % n
                if (bins[idx] > best) best = bins[idx]
            }
            rowDb[x] = best
        }
        return rowDb
    }

    override fun onDraw(canvas: Canvas) {
        val w = width
        val h = height
        if (w <= 0 || h <= 0) return
        canvas.drawColor(Color.rgb(12, 16, 22))

        SdrEngine.nativeGetSpectrum(bins)
        val row = sampleRow(w)

        val spectrumHeight = (h * 0.4f).toInt()
        val plotBottom = spectrumHeight - 2

        // Spectre : un point par colonne.
        path.reset()
        for (x in 0 until w) {
            val y = plotBottom - normalized(row[x]) * (plotBottom - 4)
            if (x == 0) path.moveTo(x.toFloat(), y) else path.lineTo(x.toFloat(), y)
        }
        paint.color = Color.rgb(120, 220, 255)
        canvas.drawPath(path, paint)

        // Marqueur de frequence centrale.
        paint.color = Color.rgb(220, 90, 60)
        canvas.drawLine(w / 2f, 0f, w / 2f, spectrumHeight.toFloat(), paint)

        // Waterfall : une nouvelle ligne en haut, le reste decale d'un pixel.
        val wfHeight = h - spectrumHeight
        if (wfHeight <= 0) return
        val bitmap = ensureWaterfall(w, wfHeight)

        val now = System.currentTimeMillis()
        val wfCanvas = waterfallCanvas
        val bufCanvas = scratchCanvas
        val buffer = scratch
        if (now - lastRowAt >= rowIntervalMs && wfCanvas != null && bufCanvas != null && buffer != null) {
            lastRowAt = now
            if (rowPixels.size != w) rowPixels = IntArray(w)
            for (x in 0 until w) rowPixels[x] = palette[(normalized(row[x]) * 255f).toInt()]
            // Copie complete puis recopie decalee : on ne dessine jamais un bitmap
            // sur lui-meme, et le tampon intermediaire est reutilise.
            bufCanvas.drawBitmap(bitmap, 0f, 0f, null)
            wfCanvas.drawBitmap(buffer, 0f, 1f, null)
            bitmap.setPixels(rowPixels, 0, w, 0, 0, w, 1)
        }

        canvas.drawBitmap(bitmap, 0f, spectrumHeight.toFloat(), null)
    }

    /** Alloue (ou reutilise) la surface du waterfall et son tampon de decalage. */
    private fun ensureWaterfall(w: Int, h: Int): Bitmap {
        val existing = waterfall
        if (existing != null && existing.width == w && existing.height == h) return existing
        val bitmap = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        bitmap.eraseColor(Color.rgb(6, 8, 14))
        val buffer = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888)
        waterfall = bitmap
        scratch = buffer
        waterfallCanvas = Canvas(bitmap)
        scratchCanvas = Canvas(buffer)
        return bitmap
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        if (event.actionMasked == MotionEvent.ACTION_DOWN) {
            val t = event.x / width.toDouble()
            onTune?.invoke((t - 0.5) * sampleRateHz)
            performClick()
            return true
        }
        return super.onTouchEvent(event)
    }

    override fun performClick(): Boolean {
        super.performClick()
        return true
    }
}
