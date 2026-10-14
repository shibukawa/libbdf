plugins {
    id("com.android.library") version "9.4.1" apply false
    id("com.android.application") version "9.4.1" apply false
    // The Android Gradle Plugin compiles Kotlin itself (AGP 9); this pins the
    // Kotlin it compiles with.
    id("org.jetbrains.kotlin.android") version "2.4.21" apply false
}
