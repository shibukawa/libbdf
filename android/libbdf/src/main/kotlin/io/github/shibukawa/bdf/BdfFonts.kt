package io.github.shibukawa.bdf

import java.io.File

/**
 * A set of fonts for the text that names its font instead of carrying it
 * (documents made from HTML, Markdown or EPUB, and characters an embedded
 * font lacks). libbdf reads no font directories: the application says
 * which fonts there are, in the order to prefer them. [system] is a set of
 * the fonts of Android itself.
 *
 * A set is not to be changed once a renderer has it, and is to be closed
 * after its renderers.
 */
class BdfFonts : AutoCloseable {
    private var handle: Long = Native.fontsNew()
    private var users = 0

    companion object {
        /** Fonts every Android has, for Latin, Greek and Cyrillic text and for Japanese, Chinese and Korean. */
        private val SYSTEM_FONTS = listOf(
            "/system/fonts/Roboto-Regular.ttf",
            "/system/fonts/RobotoFlex-Regular.ttf",
            "/system/fonts/NotoSerif-Regular.ttf",
            "/system/fonts/DroidSansMono.ttf",
            "/system/fonts/CutiveMono.ttf",
            "/system/fonts/NotoSansCJK-Regular.ttc",
            "/system/fonts/NotoSerifCJK-Regular.ttc",
            "/system/fonts/DroidSansFallback.ttf",
        )

        /** The fonts of the system that are there, of [SYSTEM_FONTS]. */
        @JvmStatic
        fun system(): BdfFonts = BdfFonts().apply {
            for (path in SYSTEM_FONTS) {
                if (File(path).canRead()) {
                    runCatching { addFile(path) }
                }
            }
        }
    }

    internal val pointer: Long
        get() = handle.takeIf { it != 0L } ?: throw IllegalStateException("the fonts are closed")

    /** Adds a font file by its path (TrueType, OpenType or a collection); its glyphs are read when drawn with. */
    fun addFile(path: String) {
        check(users == 0) { "a renderer has these fonts" }
        Native.fontsAddFile(pointer, path.utf8())
    }

    /** Adds a font file that is in memory; the set keeps a copy. */
    fun add(data: ByteArray) {
        check(users == 0) { "a renderer has these fonts" }
        Native.fontsAdd(pointer, data)
    }

    /** The number of faces in the set. */
    val count: Int get() = Native.fontsCount(pointer)

    internal fun retain() = synchronized(this) { pointer; users++ }

    internal fun release() = synchronized(this) { users-- }

    override fun close() {
        synchronized(this) {
            check(users == 0) { "close the renderers of the fonts first" }
            if (handle != 0L) {
                Native.fontsFree(handle)
                handle = 0
            }
        }
    }
}
