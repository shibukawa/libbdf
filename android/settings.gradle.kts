// libbdf for Android: the library (an AAR with the native code of each ABI
// and its Kotlin classes) and a sample viewer that uses it.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "libbdf-android"
include(":libbdf", ":sample")
