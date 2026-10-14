import org.jetbrains.kotlin.gradle.dsl.JvmTarget

plugins {
    id("com.android.application")
}

android {
    namespace = "io.github.shibukawa.bdf.sample"
    compileSdk = 35
    // the NDK that strips the debugging information of the library's native
    // code when the app is packaged
    ndkVersion = "27.3.13750724"

    defaultConfig {
        applicationId = "io.github.shibukawa.bdf.sample"
        minSdk = 24
        targetSdk = 35
        versionCode = 1
        versionName = "1.0"
    }

    buildTypes {
        release {
            // R8 as an application of the library would run it, with the
            // library's consumer rules; signed with the debug key, as a
            // sample to install
            isMinifyEnabled = true
            proguardFiles(getDefaultProguardFile("proguard-android-optimize.txt"))
            signingConfig = signingConfigs.getByName("debug")
        }
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
}

kotlin {
    compilerOptions {
        jvmTarget.set(JvmTarget.JVM_17)
    }
}

dependencies {
    implementation(project(":libbdf"))
}
