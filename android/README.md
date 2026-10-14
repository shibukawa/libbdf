# libbdf for Android

BDF documents in Android apps, from Kotlin or Java: libbdf built for each ABI with the NDK, and Kotlin classes over it. Pages are drawn by libbdf's own rasterizer into `Bitmap`s, the same pixels as the reference implementation draws, on every device.

| Directory | What |
|---|---|
| `libbdf/` | the library: `io.github.shibukawa:libbdf`, an AAR with `libbdf_jni.so` for arm64-v8a, armeabi-v7a and x86_64 (minimum API 24, pages of 16 KB) |
| `sample/` | a viewer: a bundled document, any the user opens, a password dialog for encrypted ones, pinch to zoom, drag to move, swipe to turn pages |

## Using it

Until it is on Maven Central, take it from a release: the AAR (`libbdf-android-<version>.aar`), or the Maven repository in a zip (`libbdf-android-<version>-maven.zip`) unpacked somewhere a build can find:

```kotlin
// settings.gradle.kts
dependencyResolutionManagement {
    repositories {
        google()
        mavenCentral()
        maven { url = uri("/path/to/libbdf-android-maven") }
    }
}

// app/build.gradle.kts
dependencies {
    implementation("io.github.shibukawa:libbdf:0.8.0")
}
```

```kotlin
import io.github.shibukawa.bdf.*

contentResolver.openFileDescriptor(uri, "r")!!.use { BdfDocument.open(it) }.use { doc ->
    if (doc.isLocked) doc.unlock(password)        // NFKC: full and half width are one password
    val view = doc.views.first { it.hasPages }
    BdfFonts.system().use { fonts ->              // for text that names its font
        BdfRenderer(doc, fonts).use { renderer ->
            val page: Bitmap = renderer.renderPage(view.index, 0, scale = 2f)   // 2 pixels a point
            // or only what the screen shows, into a bitmap of yours:
            renderer.renderRegion(view.index, 0, RectF(0f, 0f, 300f, 200f), bitmap)
        }
    }
    val hits = doc.search(view.index, "ｂｄｆ")    // width, case, kana, accents, as the viewer finds them
}
```

| Class | What |
|---|---|
| `BdfDocument` | `open(ByteArray)`, `open(File)`, `open(ParcelFileDescriptor)` (read where the reader asks, never whole); `isLocked`, `unlock`; `views`, `pageSize`, `sheetSize`; `title`, `dublinCore`; `text(view)`, `search(view, query)` |
| `BdfRenderer` | `renderPage(view, page, scale)`: a page as a new bitmap; `renderRegion(view, page, region, into)`: a rectangle of a page, of a scroll view's strips or of a sheet into an ARGB_8888 bitmap; `warnings` |
| `BdfFonts` | `system()`: Roboto, Noto Serif, Noto CJK and the monospaced fonts of Android; `addFile`, `add` for fonts of the app's own |
| `BdfSearch` | a search of a view asked more than once |
| `BdfException` | libbdf's status (`FORMAT`, `PASSWORD`, `TOO_LARGE`, ...) and message; `isWrongPassword` |

Close what is opened (`use { }`): a renderer before its document and its fonts. Closing a document a renderer still reads is refused with an `IllegalStateException`, not a crash. A document may be read from several threads once it is unlocked; a renderer is for one thread at a time, and the sample draws on a thread of its own.

## Building and testing

The Android SDK with platform 35, the NDK 27.3.13750724 and its CMake 3.22.1 (the Android Gradle Plugin installs the last when it can), and a JDK of 17 or later:

```bash
cd android
./gradlew :libbdf:assembleRelease :sample:assembleRelease
./gradlew :libbdf:connectedDebugAndroidTest       # on a device or an emulator
./gradlew :libbdf:publishReleasePublicationToBuildRepository   # the Maven repository, in libbdf/build/repo
```

The tests open, unlock, search and draw the documents of `libbdf/src/androidTest/assets`. The pictures they draw are compared by their SHA-256 with what libbdf draws on a desktop: on an arm64 emulator here and an x86_64 one in CI, they are the same to the byte. CI builds both modules (the sample with R8, which keeps what the JNI layer finds by name through the library's consumer rules) and runs the tests on an emulator; a release attaches the AAR and the Maven repository.

## Maven Central

A tag `v<version>` uploads the library to Maven Central (the job `maven-central` of `.github/workflows/release.yml`): the release build is signed, bundled and sent to the Central Portal, where the deployment waits, validated, for its Publish button. What it needs, as secrets of the repository:

| Secret | What |
|---|---|
| `CENTRAL_USERNAME`, `CENTRAL_PASSWORD` | a user token of central.sonatype.com (Account, Generate User Token) |
| `MAVEN_SIGNING_KEY` | the private key that signs, ASCII-armoured (`gpg --armor --export-secret-keys <id>`); its public key on a key server Central reads (keys.openpgp.org, keyserver.ubuntu.com) |
| `MAVEN_SIGNING_PASSWORD` | its passphrase |

Without them the job says so and uploads nothing. The group, `io.github.shibukawa`, is set in `libbdf/build.gradle.kts`: it has to be a namespace verified for the account of the token.

By hand, the same:

```bash
cd android
ORG_GRADLE_PROJECT_signingKey="$(gpg --armor --export-secret-keys <id>)" ORG_GRADLE_PROJECT_signingPassword=... \
  ./gradlew :libbdf:publishReleasePublicationToBuildRepository
../packaging/maven-central.sh bundle libbdf/build/repo bundle.zip
CENTRAL_USERNAME=... CENTRAL_PASSWORD=... ../packaging/maven-central.sh upload bundle.zip
```

`CENTRAL_PUBLISHING=AUTOMATIC` publishes without waiting for the button.
