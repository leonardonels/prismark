// Prismark for Android: a Jetpack Compose front-end for the C core (bindings/android).
// The native library and the fonts are built by tools/build-android.sh, which also runs this build.
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
rootProject.name = "prismark-android"
include(":app")
