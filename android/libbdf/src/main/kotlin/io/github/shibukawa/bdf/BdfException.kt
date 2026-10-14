package io.github.shibukawa.bdf

/**
 * What libbdf could not do: [status] is its status (the constants below)
 * and the message what it said.
 */
class BdfException(val status: Int, message: String) : Exception(message) {
    /** The password, or the key, does not open the document. */
    val isWrongPassword: Boolean get() = status == PASSWORD

    companion object {
        /** Malformed data. */
        const val FORMAT = 1
        /** A bad argument: a page or a view out of range, say. */
        const val ARGUMENT = 2
        const val NO_MEMORY = 3
        /** Well-formed, but beyond this library: a font collection in a part, say. */
        const val UNSUPPORTED = 4
        /** The password, or the key, does not open the document. */
        const val PASSWORD = 5
        /** An image of more pixels than a renderer draws: ask for a smaller one. */
        const val TOO_LARGE = 6
    }
}
