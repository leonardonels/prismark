/*
 * Kotlin entry point for the Android app (phase 4). Load the library from
 * the app's jniLibs (libprismark_jni.so and libprismark-kernels-*.so).
 *
 * Measurement isolation (spec 7.1): call run() from a worker thread and show
 * a static screen with no animations or timers until it returns.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
package org.prismark

object Prismark {
    init {
        System.loadLibrary("prismark_jni")
    }

    /** Mode bits, as in prismark.h. */
    const val COLD_BURST = 1 shl 0
    const val PERIODIC = 1 shl 1
    const val ST_BURST = 1 shl 2
    const val ST_SUSTAINED = 1 shl 3
    const val MC_THREADED = 1 shl 4
    const val MC_INSTANCES = 1 shl 5
    const val ALL = COLD_BURST or PERIODIC or ST_BURST or ST_SUSTAINED or MC_THREADED or MC_INSTANCES

    fun interface Listener {
        /** kind: 0 phase, 1 info, 2 warning. tempC is NaN when unknown. Called between measurement windows. */
        fun onEvent(kind: Int, phase: String?, kernel: String?, message: String?, step: Int, steps: Int, tempC: Double)
    }

    val version: String get() = nativeVersion()

    /** Blocking; returns the result document (JSON), also for a cancelled run. */
    fun run(modes: Int = ALL, kernels: String? = null, quick: Boolean = false, listener: Listener? = null): String =
        nativeRun(modes, kernels, quick, listener)

    fun cancel() = nativeCancel()

    /** Compares two result documents; throws IllegalArgumentException with the reason if they are not comparable. */
    fun compare(a: String, b: String, profiles: String? = null): String = nativeCompare(a, b, profiles)

    @JvmStatic private external fun nativeVersion(): String
    @JvmStatic private external fun nativeRun(modes: Int, kernels: String?, quick: Boolean, listener: Listener?): String
    @JvmStatic private external fun nativeCancel()
    @JvmStatic private external fun nativeCompare(a: String, b: String, profiles: String?): String
}
