package io.github.shibukawa.bdf.sample

import android.content.Context
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.os.Handler
import android.os.Looper
import android.view.GestureDetector
import android.view.MotionEvent
import android.view.ScaleGestureDetector
import android.view.View
import io.github.shibukawa.bdf.BdfDocument
import io.github.shibukawa.bdf.BdfFonts
import io.github.shibukawa.bdf.BdfRenderer
import io.github.shibukawa.bdf.BdfView
import io.github.shibukawa.bdf.BdfViewKind
import java.util.concurrent.Executors
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

/**
 * Shows a view of a document: a page of a fixed or flow view, the strips of
 * a scroll view one under the other, or a sheet. Only what the screen shows
 * is drawn, at the size it is shown (BdfRenderer.renderRegion), on a thread
 * of its own; while a drawing is under way the last one is shown moved and
 * scaled, so that pinching and dragging follow the finger.
 */
class PageView(context: Context) : View(context) {
    /** Told when the page or the view changes. */
    var onChange: (() -> Unit)? = null

    private val worker = Executors.newSingleThreadExecutor()
    private val main = Handler(Looper.getMainLooper())

    // what the worker thread draws with, and closes
    private var document: BdfDocument? = null
    private var fonts: BdfFonts? = null
    private var renderer: BdfRenderer? = null

    var view: BdfView? = null
        private set
    var page = 0
        private set

    // what is shown: the content's size in units, pixels a unit, and the
    // unit at the top left of the view
    private var contentWidth = 0f
    private var contentHeight = 0f
    private var zoom = 1f
    private var originX = 0f
    private var originY = 0f
    private var fit = true

    /** A drawing: the region of the view it shows, in units. */
    private class Frame(val bitmap: Bitmap, val region: RectF)

    private var frame: Frame? = null
    private var drawing = false
    private var again = false
    private val paint = Paint(Paint.FILTER_BITMAP_FLAG)

    /** Shows a document, from its first view that shows something. The view owns it from now on. */
    fun show(doc: BdfDocument) {
        val old = Triple(document, fonts, renderer)
        worker.execute {
            old.third?.close()
            old.second?.close()
            old.first?.close()
        }
        document = doc
        // fonts for text that names its font; drawing them is the worker's
        fonts = BdfFonts.system()
        renderer = BdfRenderer(doc, fonts)
        frame = null
        showView(doc.views.firstOrNull { it.hasPages || it.kind == BdfViewKind.SHEET } ?: return)
    }

    val views: List<BdfView> get() = document?.views.orEmpty()

    fun showView(v: BdfView, at: Int = 0) {
        view = v
        page = if (v.hasPages) at.coerceIn(0, v.pageCount - 1) else 0
        measureContent()
        fitToView()
        onChange?.invoke()
    }

    fun turn(by: Int) {
        val v = view ?: return
        if (v.hasPages && page + by in 0 until v.pageCount) {
            showView(v, page + by)
        }
    }

    private fun measureContent() {
        val doc = document ?: return
        val v = view ?: return
        contentWidth = 0f
        contentHeight = 0f
        when (v.kind) {
            BdfViewKind.SHEET -> doc.sheetSize(v.index).let { contentWidth = it.width; contentHeight = it.height }
            BdfViewKind.SCROLL -> for (p in 0 until v.pageCount) {
                doc.pageSize(v.index, p)?.let { contentWidth = max(contentWidth, it.width); contentHeight += it.height }
            }
            else -> doc.pageSize(v.index, page)?.let { contentWidth = it.width; contentHeight = it.height }
        }
    }

    private val paged: Boolean get() = view?.let { it.kind != BdfViewKind.SHEET && it.kind != BdfViewKind.SCROLL } ?: false

    /** A page whole in the view; a sheet or a scroll view across its width. */
    private fun fitToView() {
        if (width == 0 || height == 0 || contentWidth <= 0f || contentHeight <= 0f) return
        val margin = 16 * resources.displayMetrics.density
        zoom = if (paged) {
            min((width - margin) / contentWidth, (height - margin) / contentHeight)
        } else {
            min(resources.displayMetrics.density, (width - margin) / contentWidth)
        }
        originX = 0f
        originY = 0f
        fit = true
        clamp()
        redraw()
    }

    /** Keeps the content in the view: centred where it is smaller. */
    private fun clamp() {
        val w = width / zoom
        val h = height / zoom
        originX = if (contentWidth <= w) (contentWidth - w) / 2 else originX.coerceIn(0f, contentWidth - w)
        originY = if (contentHeight <= h) (contentHeight - h) / 2 else originY.coerceIn(0f, contentHeight - h)
    }

    private fun zoomAt(factor: Float, px: Float, py: Float) {
        val ux = originX + px / zoom
        val uy = originY + py / zoom
        zoom = (zoom * factor).coerceIn(0.05f, 32f)
        originX = ux - px / zoom
        originY = uy - py / zoom
        fit = false
        clamp()
        redraw()
    }

    /** Asks the worker for a drawing of what the view shows now. */
    private fun redraw() {
        invalidate()
        if (drawing) {
            again = true
            return
        }
        val r = renderer ?: return
        val v = view ?: return
        // the content inside the view, in whole pixels of the view
        val x0 = max(originX, 0f)
        val y0 = max(originY, 0f)
        val x1 = min(originX + width / zoom, contentWidth)
        val y1 = min(originY + height / zoom, contentHeight)
        if (x1 <= x0 || y1 <= y0) return
        val px0 = max(floor((x0 - originX) * zoom), 0f)
        val py0 = max(floor((y0 - originY) * zoom), 0f)
        val px1 = min(ceil((x1 - originX) * zoom), width.toFloat())
        val py1 = min(ceil((y1 - originY) * zoom), height.toFloat())
        val w = (px1 - px0).toInt()
        val h = (py1 - py0).toInt()
        if (w <= 0 || h <= 0) return
        val region = RectF(originX + px0 / zoom, originY + py0 / zoom, 0f, 0f).apply {
            right = left + w / zoom
            bottom = top + h / zoom
        }
        val index = v.index
        val p = page
        drawing = true
        worker.execute {
            val drawn = runCatching {
                Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888).also { r.renderRegion(index, p, region, it) }
            }
            main.post {
                drawing = false
                drawn.onSuccess { if (renderer === r) frame = Frame(it, region) }
                    .onFailure { android.util.Log.w("BdfViewer", "page ${p + 1}: ${it.message}") }
                invalidate()
                if (again) {
                    again = false
                    redraw()
                }
            }
        }
    }

    override fun onSizeChanged(w: Int, h: Int, oldw: Int, oldh: Int) {
        if (fit) fitToView() else { clamp(); redraw() }
    }

    override fun onDraw(canvas: Canvas) {
        canvas.drawColor(Color.rgb(0x55, 0x58, 0x5e))
        val f = frame ?: return
        // the drawing where its region is now: moved and scaled while the next is drawn
        val dst = RectF(
            (f.region.left - originX) * zoom, (f.region.top - originY) * zoom,
            (f.region.right - originX) * zoom, (f.region.bottom - originY) * zoom,
        )
        canvas.drawBitmap(f.bitmap, null, dst, paint)
    }

    private val scaler = ScaleGestureDetector(context, object : ScaleGestureDetector.SimpleOnScaleGestureListener() {
        override fun onScale(d: ScaleGestureDetector): Boolean {
            zoomAt(d.scaleFactor, d.focusX, d.focusY)
            return true
        }
    })

    private val gestures = GestureDetector(context, object : GestureDetector.SimpleOnGestureListener() {
        override fun onDown(e: MotionEvent) = true

        override fun onScroll(e1: MotionEvent?, e2: MotionEvent, dx: Float, dy: Float): Boolean {
            originX += dx / zoom
            originY += dy / zoom
            fit = false
            clamp()
            redraw()
            return true
        }

        override fun onFling(e1: MotionEvent?, e2: MotionEvent, vx: Float, vy: Float): Boolean {
            // a page that fits turns with a swipe
            if (fit && paged && kotlin.math.abs(vx) > 2 * kotlin.math.abs(vy)) {
                turn(if (vx < 0) 1 else -1)
                return true
            }
            return false
        }

        override fun onDoubleTap(e: MotionEvent): Boolean {
            if (fit) zoomAt(2.5f, e.x, e.y) else fitToView()
            return true
        }
    })

    override fun onTouchEvent(event: MotionEvent): Boolean {
        scaler.onTouchEvent(event)
        if (!scaler.isInProgress) gestures.onTouchEvent(event)
        return true
    }

    /** Closes the document and what draws it, after the drawing under way. */
    fun close() {
        val old = Triple(document, fonts, renderer)
        document = null
        fonts = null
        renderer = null
        worker.execute {
            old.third?.close()
            old.second?.close()
            old.first?.close()
        }
        worker.shutdown()
    }
}
