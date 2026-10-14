package io.github.shibukawa.bdf.sample

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.Color
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.text.InputType
import android.view.Gravity
import android.view.ViewGroup.LayoutParams.MATCH_PARENT
import android.view.ViewGroup.LayoutParams.WRAP_CONTENT
import android.view.WindowInsets
import android.widget.Button
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.TextView
import android.widget.Toast
import io.github.shibukawa.bdf.BdfDocument
import io.github.shibukawa.bdf.BdfException
import io.github.shibukawa.bdf.BdfViewKind

/**
 * A viewer of BDF documents with libbdf for Android: a bundled document at
 * first, and any the user opens. An encrypted document asks for its
 * password. Pinch to zoom, drag to move, swipe or the buttons to turn the
 * pages, double tap to zoom in and back.
 */
class MainActivity : Activity() {
    private lateinit var pages: PageView
    private lateinit var label: TextView
    private var name = ""

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        pages = PageView(this)
        label = TextView(this).apply {
            gravity = Gravity.CENTER
            setTextColor(Color.WHITE)
            isSingleLine = true
        }
        fun button(text: String, action: () -> Unit) = Button(this).apply {
            this.text = text
            isAllCaps = false
            setOnClickListener { action() }
        }
        val bar = LinearLayout(this).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setBackgroundColor(Color.rgb(0x2b, 0x2d, 0x31))
            addView(button("Open") { choose() })
            addView(button("View") { nextView() })
            addView(button("‹") { pages.turn(-1) })
            addView(label, LinearLayout.LayoutParams(0, WRAP_CONTENT, 1f))
            addView(button("›") { pages.turn(1) })
        }
        val root = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setBackgroundColor(Color.rgb(0x2b, 0x2d, 0x31))
            addView(bar, LinearLayout.LayoutParams(MATCH_PARENT, WRAP_CONTENT))
            addView(pages, LinearLayout.LayoutParams(MATCH_PARENT, 0, 1f))
        }
        // drawn edge to edge (Android 15): keep clear of the system bars
        root.setOnApplyWindowInsetsListener { v, insets ->
            if (Build.VERSION.SDK_INT >= 30) {
                val bars = insets.getInsets(WindowInsets.Type.systemBars() or WindowInsets.Type.displayCutout())
                v.setPadding(bars.left, bars.top, bars.right, bars.bottom)
            } else {
                @Suppress("DEPRECATION")
                v.setPadding(insets.systemWindowInsetLeft, insets.systemWindowInsetTop, insets.systemWindowInsetRight, insets.systemWindowInsetBottom)
            }
            insets
        }
        setContentView(root)
        pages.onChange = { update() }

        // the bundled document: a deck, a document of pages and a sheet
        open("demo.bdf") { BdfDocument.open(assets.open("demo.bdf").use { it.readBytes() }) }
    }

    override fun onDestroy() {
        pages.close()
        super.onDestroy()
    }

    private fun choose() {
        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "*/*"
        }
        @Suppress("DEPRECATION")
        startActivityForResult(intent, OPEN)
    }

    @Deprecated("Deprecated in Java")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        @Suppress("DEPRECATION")
        super.onActivityResult(requestCode, resultCode, data)
        val uri: Uri = data?.data ?: return
        if (requestCode == OPEN && resultCode == RESULT_OK) {
            // read where the reader asks, through the descriptor of the document chosen
            open(uri.lastPathSegment?.substringAfterLast('/') ?: "document") {
                contentResolver.openFileDescriptor(uri, "r")!!.use { BdfDocument.open(it) }
            }
        }
    }

    private fun open(title: String, opener: () -> BdfDocument) {
        val doc = try {
            opener()
        } catch (e: Exception) {
            Toast.makeText(this, "$title: ${e.message}", Toast.LENGTH_LONG).show()
            return
        }
        name = title
        if (doc.isLocked) askPassword(doc) else pages.show(doc)
    }

    /** An encrypted document: its views come with the password. */
    private fun askPassword(doc: BdfDocument, again: Boolean = false) {
        val field = EditText(this).apply {
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_VARIATION_PASSWORD
            hint = "Password"
        }
        AlertDialog.Builder(this)
            .setTitle(name)
            .setMessage(if (again) "The password does not open it. Try again." else "The document is encrypted.")
            .setView(field)
            .setPositiveButton("Open") { _, _ ->
                try {
                    doc.unlock(field.text.toString())
                    pages.show(doc)
                } catch (e: BdfException) {
                    if (e.isWrongPassword) askPassword(doc, again = true) else fail(doc, e)
                }
            }
            .setNegativeButton("Cancel") { _, _ -> doc.close() }
            .setCancelable(false)
            .show()
    }

    private fun fail(doc: BdfDocument, e: Exception) {
        doc.close()
        Toast.makeText(this, "$name: ${e.message}", Toast.LENGTH_LONG).show()
    }

    /** The next view that shows something: of pages, or a sheet. */
    private fun nextView() {
        val views = pages.views.filter { it.hasPages || it.kind == BdfViewKind.SHEET }
        if (views.size < 2) return
        val i = views.indexOfFirst { it.index == pages.view?.index }
        pages.showView(views[(i + 1) % views.size])
    }

    private fun update() {
        val v = pages.view ?: return
        val title = v.title.ifEmpty { v.id }
        label.text = if (v.hasPages && v.kind != BdfViewKind.SCROLL) "$title  ${pages.page + 1} / ${v.pageCount}" else title
    }

    private companion object {
        const val OPEN = 1
    }
}
