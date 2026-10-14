package io.github.shibukawa.bdf

import android.os.ParcelFileDescriptor
import java.io.File

/** The kind of a view (spec §4). */
enum class BdfViewKind { UNKNOWN, FIXED, FLOW, SHEET, SCROLL }

/**
 * A view of a document: a deck of slides, the pages of a book, a sheet.
 * [index] is what the other functions take.
 */
data class BdfView(
    val index: Int,
    val id: String,
    val kind: BdfViewKind,
    val title: String,
    val pageCount: Int,
    val rightToLeft: Boolean,
    val hasTextIndex: Boolean,
) {
    /** Whether the view has pages to draw one at a time: not a sheet. */
    val hasPages: Boolean get() = pageCount > 0 && kind != BdfViewKind.SHEET
}

/** A size in the units of a document: points. */
data class BdfSize(val width: Float, val height: Float)

/**
 * A hit of a search: [text] as the document has it, [context] the
 * searchable text around it (a paragraph break shows as " ⏎ "), and where
 * it lies: [page] from 0 in a view of pages, or the tile ([tileX], [tileY])
 * of a sheet.
 */
data class BdfSearchHit(
    val text: String,
    val context: String,
    val page: Int,
    val tileX: Int,
    val tileY: Int,
)

/**
 * A BDF document. Open it with [open]; close it when it is done with,
 * after the renderers and searches of it ([use] does that).
 *
 * A document may be read from several threads once it is unlocked; a
 * [BdfRenderer] is for one thread at a time.
 */
class BdfDocument private constructor(handle: Long) : AutoCloseable {
    private var handle = handle
    private var users = 0 // the renderers and searches that read it

    companion object {
        /** Opens a document held in memory. */
        @JvmStatic
        fun open(bytes: ByteArray): BdfDocument = BdfDocument(Native.openBytes(bytes))

        /**
         * Opens a document file. It is read where the reader asks, so a large
         * one is not read whole; the file stays open until the document is
         * closed.
         */
        @JvmStatic
        fun open(file: File): BdfDocument =
            ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { open(it) }

        /**
         * Opens a document from a file descriptor: what
         * `ContentResolver.openFileDescriptor(uri, "r")` gives for a document
         * the user chose. The document keeps a copy of the descriptor: the
         * one given may be closed.
         */
        @JvmStatic
        fun open(descriptor: ParcelFileDescriptor): BdfDocument = BdfDocument(Native.openFd(descriptor.fd))

        /** The version of libbdf. */
        @JvmStatic
        val libraryVersion: String
            get() = Native.version().utf8()
    }

    internal val pointer: Long
        get() = handle.takeIf { it != 0L } ?: throw IllegalStateException("the document is closed")

    /** Whether the document is encrypted. */
    val isEncrypted: Boolean get() = Native.encrypted(pointer)

    /** Whether it is encrypted and not unlocked yet: it has no views then. */
    val isLocked: Boolean get() = Native.locked(pointer)

    /**
     * Unlocks an encrypted document with its password. The password is taken
     * in NFKC, as the format asks: full-width and half-width spellings are
     * one password. A wrong one throws a [BdfException] whose
     * [BdfException.isWrongPassword] is true; another can be tried.
     */
    fun unlock(password: String) = Native.unlock(pointer, password.utf8())

    /** The views, in the order of the document. */
    val views: List<BdfView>
        get() = (0 until Native.viewCount(pointer)).mapNotNull { i ->
            val n = Native.viewNumbers(pointer, i) ?: return@mapNotNull null
            val s = Native.viewStrings(pointer, i) ?: return@mapNotNull null
            BdfView(
                index = i,
                id = s[0].utf8(),
                kind = BdfViewKind.entries.getOrElse(n[0].toInt()) { BdfViewKind.UNKNOWN },
                title = s[1].utf8(),
                pageCount = n[1].toInt(),
                rightToLeft = n[2] != 0L,
                hasTextIndex = n[3] != 0L,
            )
        }

    /** The size of a page of a view (or of a strip of a scroll view), or null. */
    fun pageSize(view: Int, page: Int): BdfSize? =
        Native.pageSize(pointer, view, page)?.let { BdfSize(it[0], it[1]) }

    /** The size of a sheet, from its rows and columns. */
    fun sheetSize(view: Int): BdfSize =
        Native.sheetSize(pointer, view).let { BdfSize(it[0].toFloat(), it[1].toFloat()) }

    /** The values of a Dublin Core element: "title", "creator", "subject", "language", ... */
    fun dublinCore(element: String): List<String> = Native.dublinCore(pointer, element.utf8()).map { it.utf8() }

    /** The first title, or null. */
    val title: String? get() = dublinCore("title").firstOrNull()

    /** The format the document was made from ("pdf", "pptx", ...), or "". */
    val source: String get() = Native.meta(pointer)[0].utf8()

    /** The text of a view in reading order: a space or a line break where its runs say. */
    fun text(view: Int): String = Native.viewText(pointer, view).utf8()

    /**
     * Finds a phrase in a view the way the viewer of the reference does:
     * characters are taken for what they are to a reader (full and half
     * width, ligatures, an accent written apart, katakana and hiragana, and
     * unless [caseSensitive] capitals and small letters), across the runs of
     * a line and across lines, but not across a paragraph break.
     */
    @JvmOverloads
    fun search(
        view: Int,
        query: String,
        caseSensitive: Boolean = false,
        limit: Int = 1000,
        context: Int = 40,
    ): List<BdfSearchHit> = BdfSearch(this, view, caseSensitive).use { it.find(query, limit, context) }

    internal fun retain() {
        synchronized(this) {
            pointer
            users++
        }
    }

    internal fun release() {
        synchronized(this) { users-- }
    }

    /** Closes the document. Its renderers and searches are to be closed first. */
    override fun close() {
        synchronized(this) {
            check(users == 0) { "close the renderers and searches of the document first" }
            if (handle != 0L) {
                Native.closeDocument(handle)
                handle = 0
            }
        }
    }
}

/**
 * A search of a view, for asking it more than once: the view's text is
 * normalised once. [BdfDocument.search] is one question.
 */
class BdfSearch(private val document: BdfDocument, view: Int, caseSensitive: Boolean = false) : AutoCloseable {
    private var handle: Long

    init {
        handle = Native.searchNew(document.pointer, view, caseSensitive)
        document.retain()
    }

    /** The hits of a query, in the order of the text: at most [limit], with [context] characters around each. */
    @JvmOverloads
    @Synchronized
    fun find(query: String, limit: Int = 1000, context: Int = 40): List<BdfSearchHit> {
        check(handle != 0L) { "the search is closed" }
        val found = Native.searchFind(handle, query.utf8(), limit, context)
        @Suppress("UNCHECKED_CAST")
        val texts = found[0] as Array<ByteArray>
        @Suppress("UNCHECKED_CAST")
        val contexts = found[1] as Array<ByteArray>
        val where = found[2] as IntArray
        return texts.indices.map { i ->
            BdfSearchHit(texts[i].utf8(), contexts[i].utf8(), where[5 * i], where[5 * i], where[5 * i + 1])
        }
    }

    @Synchronized
    override fun close() {
        if (handle != 0L) {
            Native.searchFree(handle)
            handle = 0
            document.release()
        }
    }
}
