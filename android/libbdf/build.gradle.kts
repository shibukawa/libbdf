import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.library")
    `maven-publish`
    signing
}

// The version of libbdf, as its header says.
val bdfVersion: String = rootDir.resolve("../include/bdf/bdf.h").readLines()
    .first { it.startsWith("#define BDF_VERSION_STRING") }
    .substringAfter('"').substringBefore('"')

android {
    namespace = "io.github.shibukawa.bdf"
    compileSdk = 35
    ndkVersion = "27.3.13750724"

    defaultConfig {
        minSdk = 24
        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"
        consumerProguardFiles("consumer-rules.pro")
        ndk {
            abiFilters += listOf("arm64-v8a", "armeabi-v7a", "x86_64")
        }
        externalNativeBuild {
            cmake {
                // pages of 16 KB, as Android 15 devices may have
                arguments += listOf("-DANDROID_SUPPORT_FLEXIBLE_PAGE_SIZES=ON")
            }
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }

    publishing {
        singleVariant("release") {
            withSourcesJar()
            withJavadocJar()
        }
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    androidTestImplementation("androidx.test:runner:1.7.0")
    androidTestImplementation("androidx.test.ext:junit:1.3.0")
    androidTestImplementation("junit:junit:4.13.2")
}

// `./gradlew :libbdf:publishReleasePublicationToBuildRepository` writes the
// AAR, its sources, its documentation and its POM as a Maven repository
// under build/repo: what a release attaches, and with signatures what goes
// to Maven Central (packaging/maven-central.sh). The group is in the
// namespace io.github.shibukawa of the publisher on Maven Central, as
// io.github.shibukawa.pgmem is.
publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "io.github.shibukawa.libbdf"
            artifactId = "libbdf"
            version = bdfVersion
            afterEvaluate {
                from(components["release"])
            }
            pom {
                name.set("libbdf")
                description.set("BDF (Browser-specific Document Format) documents on Android: libbdf, the C reader and renderer, with Kotlin classes over it.")
                url.set("https://github.com/shibukawa/libbdf")
                licenses {
                    license {
                        name.set("MIT")
                        url.set("https://github.com/shibukawa/libbdf/blob/main/LICENSE")
                    }
                }
                developers {
                    developer {
                        id.set("shibukawa")
                        name.set("Yoshiki Shibukawa")
                        url.set("https://github.com/shibukawa")
                    }
                }
                scm {
                    url.set("https://github.com/shibukawa/libbdf")
                    connection.set("scm:git:https://github.com/shibukawa/libbdf.git")
                    developerConnection.set("scm:git:ssh://git@github.com/shibukawa/libbdf.git")
                }
            }
        }
    }
    repositories {
        maven {
            name = "build"
            url = uri(layout.buildDirectory.dir("repo"))
        }
    }
}

// Signatures, which Maven Central asks for: with a key in the Gradle
// properties signingKey (ASCII-armoured) and signingPassword, as the release
// workflow gives them from its secrets GPG_PRIVATE_KEY and GPG_PASSPHRASE
// (ORG_GRADLE_PROJECT_signingKey, ORG_GRADLE_PROJECT_signingPassword), every file of the publication gets
// its .asc. Without a key (or with an empty one) nothing is signed.
signing {
    val key = providers.gradleProperty("signingKey").orNull?.takeIf { it.isNotBlank() }
    if (key != null) {
        useInMemoryPgpKeys(key, providers.gradleProperty("signingPassword").orNull)
        sign(publishing.publications)
    }
}
