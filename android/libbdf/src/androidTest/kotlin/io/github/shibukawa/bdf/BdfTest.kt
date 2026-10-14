package io.github.shibukawa.bdf

import android.graphics.Bitmap
import android.graphics.RectF
import android.os.ParcelFileDescriptor
import androidx.test.ext.junit.runners.AndroidJUnit4
import androidx.test.platform.app.InstrumentationRegistry
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import java.io.File
import java.nio.ByteBuffer
import java.security.MessageDigest

/**
 * libbdf on a device: the documents of the assets opened, unlocked,
 * searched and drawn. libbdf draws the same pixels on every platform, so
 * the hashes of the pictures are those libbdf draws on a desktop.
 */
@RunWith(AndroidJUnit4::class)
class BdfTest {
    private val context = InstrumentationRegistry.getInstrumentation().context

    private fun asset(name: String): ByteArray = context.assets.open(name).use { it.readBytes() }

    private fun sha256(bitmap: Bitmap): String {
        val buffer = ByteBuffer.allocate(bitmap.byteCount)
        bitmap.copyPixelsToBuffer(buffer) // red, green, blue and alpha, premultiplied, as libbdf draws
        return MessageDigest.getInstance("SHA-256").digest(buffer.array()).joinToString("") { "%02x".format(it) }
    }

    @Test
    fun opensAndDescribes() {
        BdfDocument.open(asset("demo.bdf")).use { doc ->
            assertFalse(doc.isEncrypted)
            assertFalse(doc.isLocked)
            assertEquals("BDF fixture", doc.title)
            assertEquals(listOf("en", "ja"), doc.dublinCore("language"))
            assertEquals("fixture", doc.source)
            val views = doc.views
            assertEquals(listOf("slides", "doc", "sheet1"), views.map { it.id })
            assertEquals(listOf(BdfViewKind.FIXED, BdfViewKind.FLOW, BdfViewKind.SHEET), views.map { it.kind })
            assertEquals(3, views[0].pageCount)
            assertTrue(views[0].hasPages)
            assertFalse(views[2].hasPages)
            assertEquals(BdfSize(960f, 540f), doc.pageSize(0, 0))
            assertNull(doc.pageSize(0, 3))
            assertEquals(BdfSize(1664f, 4000f), doc.sheetSize(2))
            assertTrue(BdfDocument.libraryVersion.isNotEmpty())
        }
    }

    @Test
    fun opensAFileWithoutReadingItWhole() {
        val file = File(context.cacheDir, "demo.bdf")
        file.writeBytes(asset("demo.bdf"))
        BdfDocument.open(file).use { doc -> assertEquals(3, doc.views.size) }
        ParcelFileDescriptor.open(file, ParcelFileDescriptor.MODE_READ_ONLY).use { pfd ->
            BdfDocument.open(pfd)
        }.use { doc ->
            // the document keeps its own descriptor: the one it was given is closed
            BdfRenderer(doc).use { r -> assertEquals(960, r.renderPage(0, 0, 1f).width) }
        }
    }

    @Test
    fun refusesWhatIsNotADocument() {
        val e = assertThrows(BdfException::class.java) { BdfDocument.open("not a document".toByteArray()) }
        assertEquals(BdfException.FORMAT, e.status)
    }

    @Test
    fun drawsTheSamePixelsAsEverywhere() {
        BdfDocument.open(asset("two-pages.bdf")).use { doc ->
            BdfRenderer(doc).use { r ->
                val page = r.renderPage(0, 0, 2f)
                assertEquals(200, page.width)
                assertEquals(160, page.height)
                assertEquals("3ad2d93ba7755e0af473face26cd1a636bb19a11d9bc999cc4c2ef16c9fa33e9", sha256(page))
            }
        }
        BdfDocument.open(asset("demo.bdf")).use { doc ->
            BdfRenderer(doc).use { r ->
                assertEquals("26abf1948864e70129a26c4c1a5013f0ae89e76ce6a9067d20d1327e81327c61", sha256(r.renderPage(0, 0, 1f)))
                assertEquals("bc7489e27e5643ef53e678925a2a053d751c5dd274a99a6e4da79a34723506d0", sha256(r.renderPage(1, 1, 1f)))
                // a region of a page is the page, drawn into the bitmap given
                val half = Bitmap.createBitmap(480, 270, Bitmap.Config.ARGB_8888)
                r.renderRegion(0, 0, RectF(0f, 0f, 960f, 540f), half)
                assertEquals("3586ba1c1a57f3bc0f39fed7d6c59db86e12c9f1689ecc9abd751f7bf43c559a", sha256(half))
                // and of a sheet
                val sheet = Bitmap.createBitmap(400, 300, Bitmap.Config.ARGB_8888)
                r.renderRegion(2, 0, RectF(0f, 0f, 400f, 300f), sheet)
                assertEquals("226f7389467e2478a622de722679990104785a33631b90ad3c58998ca189c355", sha256(sheet))
                assertTrue(r.warnings.isEmpty())
                assertEquals(BdfException.ARGUMENT, assertThrows(BdfException::class.java) { r.renderPage(0, 3, 1f) }.status)
            }
        }
    }

    @Test
    fun drawsTextThatNamesItsFontWithTheFontsOfTheSystem() {
        BdfDocument.open(asset("markdown.bdf")).use { doc ->
            BdfRenderer(doc).use { r ->
                r.renderPage(0, 0, 1f)
                assertTrue(r.warnings.toString(), r.warnings.any { it.startsWith("no font for") })
            }
            BdfFonts.system().use { fonts ->
                assertTrue(fonts.count > 0)
                BdfRenderer(doc, fonts).use { r ->
                    r.renderPage(0, 0, 1f)
                    assertFalse(r.warnings.toString(), r.warnings.any { it.startsWith("no font for") })
                    // a set a renderer has is not to be changed
                    assertThrows(IllegalStateException::class.java) { fonts.addFile("/system/fonts/Roboto-Regular.ttf") }
                }
            }
        }
    }

    @Test
    fun unlocksWithThePasswordInNFKC() {
        BdfDocument.open(asset("demo-encrypted.bdf")).use { doc ->
            assertTrue(doc.isEncrypted)
            assertTrue(doc.isLocked)
            assertTrue(doc.views.isEmpty())
            val wrong = assertThrows(BdfException::class.java) { doc.unlock("demo-ぱすわーど") }
            assertTrue(wrong.isWrongPassword)
            assertTrue(doc.isLocked)
            // full-width letters and hyphen, half-width katakana: the same password
            doc.unlock("ｄｅｍｏ－ﾊﾟｽﾜｰﾄﾞ")
            assertFalse(doc.isLocked)
            assertEquals(3, doc.views.size)
            BdfRenderer(doc).use { r ->
                assertEquals("26abf1948864e70129a26c4c1a5013f0ae89e76ce6a9067d20d1327e81327c61", sha256(r.renderPage(0, 0, 1f)))
            }
        }
    }

    @Test
    fun findsTextAsAReaderTakesIt() {
        BdfDocument.open(asset("demo.bdf")).use { doc ->
            assertTrue(doc.text(0).startsWith("BDF fixture deck\nShapes and paths"))
            // full-width letters find the half-width ones
            val hits = doc.search(0, "ｂｄｆ")
            assertEquals(listOf(0, 1, 2), hits.map { it.page })
            assertEquals("BDF", hits[0].text)
            assertTrue(hits[0].context, hits[0].context.contains("bdf fixture deck"))
            assertTrue(doc.search(0, "bdf", caseSensitive = true).isEmpty())
            BdfSearch(doc, 1).use { search ->
                assertEquals(8, search.find("BDF").size)
                assertEquals(listOf(0, 1), search.find("section").map { it.page })
            }
        }
    }

    @Test
    fun aDocumentOutlivesItsRenderers() {
        val doc = BdfDocument.open(asset("two-pages.bdf"))
        val r = BdfRenderer(doc)
        assertThrows(IllegalStateException::class.java) { doc.close() }
        r.close()
        doc.close()
        assertThrows(IllegalStateException::class.java) { doc.views }
        assertNotNull(doc) // closing twice is nothing
        doc.close()
    }
}
