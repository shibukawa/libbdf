import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.library")
    `maven-publish`
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
// AAR, its sources and its POM as a Maven repository under build/repo: what
// a release attaches, and what would go to Maven Central.
publishing {
    publications {
        register<MavenPublication>("release") {
            groupId = "io.github.shibukawa"
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
                scm {
                    url.set("https://github.com/shibukawa/libbdf")
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
