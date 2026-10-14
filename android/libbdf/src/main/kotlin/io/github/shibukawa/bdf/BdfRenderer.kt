package io.github.shibukawa.bdf

import android.graphics.Bitmap
import android.graphics.RectF

/**
 * Draws the views of a document into bitmaps with libbdf's own rasterizer:
 * the same pixels as the reference implementation draws, on every device.
 * A renderer keeps what it decodes for the next drawing. Use it from one
 * thread at a time (its functions are synchronized), and close it before
 * the document and the fonts.
 *
 * @param fonts the fonts for text that names its font, or null: such text
 *   is then not drawn, with a warning
 * @param background the colour behind the content as 0xAARRGGBB, or null
 *   for white
 */
class BdfRenderer @JvmOverloads constructor(
    val document: BdfDocument,
    val fonts: BdfFonts? = null,
    background: Int? = null,
) : AutoCloseable {
    private var handle: Long

    init {
        // libbdf takes the colour as 0xRRGGBBAA
        val rgba = background?.let { (it shl 8) or (it ushr 24) } ?: 0
        handle = Native.rendererNew(document.pointer, fonts?.pointer ?: 0L, background != null, rgba)
        document.retain()
        fonts?.retain()
    }

    private val pointer: Long
        get() = handle.takeIf { it != 0L } ?: throw IllegalStateException("the renderer is closed")

    /**
     * Draws a whole page of a fixed or flow view, or a strip of a scroll
     * view, at [scale] pixels a unit. The units of a page are points: a scale
     * of `dpi / 72f` draws at dpi pixels an inch.
     */
    @Synchronized
    fun renderPage(view: Int, page: Int, scale: Float): Bitmap = Native.renderPage(pointer, view, page, scale.toDouble())

    /**
     * Draws [region] of a view (in its units) into the whole of [into], an
     * ARGB_8888 bitmap: a part of a page, of the strips of a scroll view
     * stacked from the top, or of a sheet ([page] is not used for the last
     * two). For a viewer that draws only what the screen shows, at the size
     * it is shown.
     */
    @Synchronized
    fun renderRegion(view: Int, page: Int, region: RectF, into: Bitmap) {
        require(into.config == Bitmap.Config.ARGB_8888 && into.isMutable) { "an ARGB_8888 bitmap that can be written" }
        Native.renderRegion(pointer, view, page, region.left, region.top, region.width(), region.height(), into)
    }

    /** What could not be drawn as the viewer of the reference would draw it, each once. */
    @get:Synchronized
    val warnings: List<String>
        get() = Native.warnings(pointer).map { it.utf8() }

    @Synchronized
    override fun close() {
        if (handle != 0L) {
            Native.rendererFree(handle)
            handle = 0
            fonts?.release()
            document.release()
        }
    }
}
