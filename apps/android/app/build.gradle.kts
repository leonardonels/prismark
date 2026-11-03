plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
    id("org.jetbrains.kotlin.plugin.compose")
}

// The version of the C core (CMakeLists.txt project()), so the app and the results it writes agree.
val prismarkVersion: String = Regex("""project\(prismark VERSION ([0-9.]+)""")
    .find(rootDir.resolve("../../CMakeLists.txt").readText())?.groupValues?.get(1) ?: "0.0.0"

android {
    namespace = "org.prismark.app"
    compileSdk = 35

    defaultConfig {
        applicationId = "org.prismark.app"
        minSdk = 28 // as the native library (cmake/toolchains/android-arm64.cmake)
        targetSdk = 35
        versionName = prismarkVersion
        versionCode = prismarkVersion.split(".").fold(0) { acc, p -> acc * 100 + (p.toIntOrNull() ?: 0) }
        ndk { abiFilters += "arm64-v8a" }
    }

    // The release key comes from the environment (docs/packaging.md); without it a release build is unsigned.
    signingConfigs {
        System.getenv("PRISMARK_ANDROID_KEYSTORE")?.let { path ->
            create("release") {
                storeFile = file(path)
                storePassword = System.getenv("PRISMARK_ANDROID_KEYSTORE_PASSWORD")
                keyAlias = System.getenv("PRISMARK_ANDROID_KEY_ALIAS")
                keyPassword = System.getenv("PRISMARK_ANDROID_KEY_PASSWORD")
            }
        }
    }
    buildTypes {
        release {
            isMinifyEnabled = false
            signingConfig = signingConfigs.findByName("release")
        }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }
    buildFeatures {
        compose = true
        buildConfig = true
    }
    sourceSets["main"].java.srcDir("../../../bindings/android") // Prismark.kt, the binding to the C core
    packaging {
        jniLibs.useLegacyPackaging = false
    }
}

dependencies {
    val composeBom = platform("androidx.compose:compose-bom:2024.12.01")
    implementation(composeBom)
    implementation("androidx.activity:activity-compose:1.9.3")
    implementation("androidx.compose.animation:animation")
    implementation("androidx.compose.foundation:foundation")
    implementation("androidx.compose.ui:ui")
}

// The library is built from the C sources by tools/build-android.sh (CMake preset android-arm64).
val checkNativeLibrary by tasks.registering {
    val lib = file("src/main/jniLibs/arm64-v8a/libprismark_jni.so")
    doLast {
        if (!lib.isFile) throw GradleException("$lib is missing: build it with tools/build-android.sh")
    }
}
tasks.named("preBuild") { dependsOn(checkNativeLibrary) }
