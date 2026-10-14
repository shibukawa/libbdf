package io.github.shibukawa.bdf

import android.graphics.Bitmap

/**
 * The functions of libbdf as the JNI layer (src/main/cpp/bdf_jni.c) gives
 * them. Handles are pointers; strings are the bytes of their UTF-8. The
 * classes of this package are what to use.
 */
internal object Native {
    init {
        System.loadLibrary("bdf_jni")
    }

    @JvmStatic external fun version(): ByteArray

    @JvmStatic external fun openBytes(data: ByteArray): Long
    @JvmStatic external fun openFd(fd: Int): Long
    @JvmStatic external fun closeDocument(doc: Long)
    @JvmStatic external fun encrypted(doc: Long): Boolean
    @JvmStatic external fun locked(doc: Long): Boolean
    @JvmStatic external fun unlock(doc: Long, password: ByteArray)
    @JvmStatic external fun viewCount(doc: Long): Int
    @JvmStatic external fun viewNumbers(doc: Long, view: Int): LongArray?
    @JvmStatic external fun viewStrings(doc: Long, view: Int): Array<ByteArray>?
    @JvmStatic external fun pageSize(doc: Long, view: Int, page: Int): FloatArray?
    @JvmStatic external fun sheetSize(doc: Long, view: Int): DoubleArray
    @JvmStatic external fun dublinCore(doc: Long, element: ByteArray): Array<ByteArray>
    @JvmStatic external fun meta(doc: Long): Array<ByteArray>

    @JvmStatic external fun viewText(doc: Long, view: Int): ByteArray
    @JvmStatic external fun searchNew(doc: Long, view: Int, caseSensitive: Boolean): Long
    @JvmStatic external fun searchFree(search: Long)
    @JvmStatic external fun searchFind(search: Long, query: ByteArray, limit: Int, context: Int): Array<Any>

    @JvmStatic external fun fontsNew(): Long
    @JvmStatic external fun fontsFree(fonts: Long)
    @JvmStatic external fun fontsAddFile(fonts: Long, path: ByteArray)
    @JvmStatic external fun fontsAdd(fonts: Long, data: ByteArray)
    @JvmStatic external fun fontsCount(fonts: Long): Int

    @JvmStatic external fun rendererNew(doc: Long, fonts: Long, hasBackground: Boolean, background: Int): Long
    @JvmStatic external fun rendererFree(renderer: Long)
    @JvmStatic external fun renderPage(renderer: Long, view: Int, page: Int, scale: Double): Bitmap
    @JvmStatic external fun renderRegion(
        renderer: Long, view: Int, page: Int, x: Float, y: Float, w: Float, h: Float, into: Bitmap,
    )
    @JvmStatic external fun warnings(renderer: Long): Array<ByteArray>
}

internal fun ByteArray.utf8(): String = String(this, Charsets.UTF_8)

internal fun String.utf8(): ByteArray = toByteArray(Charsets.UTF_8)
