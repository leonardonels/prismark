/*
 * JNI bridge for the Android app: org.prismark.Prismark (see Prismark.kt).
 * The runner executes in-process (spec 7.1); the app must show its frozen,
 * animation-free screen while nativeRun is in progress. Progress events
 * arrive on the calling thread, between measurement windows.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <jni.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "prismark/prismark.h"

typedef struct listener {
  JNIEnv *env;
  jobject obj;
  jmethodID on_event;
  pthread_t thread; /* the thread that called nativeRun: env is valid only there */
} listener;

static jstring new_string(JNIEnv *env, const char *s) { return s ? (*env)->NewStringUTF(env, s) : NULL; }

static void throw_new(JNIEnv *env, const char *cls, const char *msg) {
  jclass ex = (*env)->FindClass(env, cls);
  if (!ex) return; /* FindClass has thrown NoClassDefFoundError */
  (*env)->ThrowNew(env, ex, msg ? msg : "error");
  (*env)->DeleteLocalRef(env, ex);
}

static void on_event(const pmk_event *ev, void *user) {
  listener *l = user;
  if (!l->obj || !l->on_event) return;
  /* The core reports from the thread that started the run; anything else would need its own JNIEnv. */
  if (!pthread_equal(pthread_self(), l->thread)) return;
  JNIEnv *env = l->env;
  jstring phase = new_string(env, ev->phase), kernel = new_string(env, ev->kernel), msg = new_string(env, ev->message);
  (*env)->CallVoidMethod(env, l->obj, l->on_event, (jint)ev->kind, phase, kernel, msg, (jint)ev->step, (jint)ev->steps,
                         (jdouble)(isfinite(ev->temp_c) ? ev->temp_c : NAN));
  if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env); /* a listener bug must not abort a run */
  if (phase) (*env)->DeleteLocalRef(env, phase);
  if (kernel) (*env)->DeleteLocalRef(env, kernel);
  if (msg) (*env)->DeleteLocalRef(env, msg);
}

JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeVersion(JNIEnv *env, jclass cls) {
  (void)cls;
  return new_string(env, pmk_version());
}

/* What this build can run here (pmk_info), as JSON. Must not be called while a run is in progress. */
JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeInfo(JNIEnv *env, jclass cls) {
  (void)cls;
  char *json = NULL;
  int rc = pmk_info(&json);
  jstring out = rc == PMK_OK ? new_string(env, json) : NULL;
  pmk_free(json);
  if (!out && !(*env)->ExceptionCheck(env)) throw_new(env, "java/lang/IllegalStateException", pmk_strerror(rc));
  return out;
}

/* Returns the result document (also for partial runs), or throws with the error. */
JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeRun(JNIEnv *env, jclass cls, jint modes, jstring kernels,
                                                               jboolean quick, jint stress_parts, jdouble stress_s,
                                                               jboolean stress_stop, jdouble stress_max_temp,
                                                               jobject listener_obj) {
  (void)cls;
  pmk_config cfg;
  pmk_config_init(&cfg); /* library defaults, including max_background_load: a busy device warns, never stops */
  cfg.modes = (uint32_t)modes;
  cfg.frontend = "android";
  cfg.ui_state = "frozen";
  cfg.input_watch = "touch"; /* the app reports touches during a run with nativeNotifyInput */
  const char *k = kernels ? (*env)->GetStringUTFChars(env, kernels, NULL) : NULL;
  cfg.kernels = k;
  if (quick) { /* as the command line's --quick */
    cfg.max_reps = 20;
    cfg.cold_max_reps = 20;
    cfg.periodic_seconds = 10;
    cfg.window_ms = 250;
    cfg.warmup_s = 0;
    cfg.settle_s = 0;
    cfg.measure_s = 3;
    cfg.k1x_reps = 1;
    cfg.cooldown = 0;
    cfg.quick_inputs = 1;
  }
  if (stress_parts) {
    cfg.stress_parts = (uint32_t)stress_parts;
    cfg.stress_s = stress_s;
    cfg.stress_stop_on_error = stress_stop ? 1 : 0;
    cfg.stress_max_temp_c = stress_max_temp;
  }
  listener l = {env, listener_obj, NULL, pthread_self()};
  if (listener_obj) {
    jclass lc = (*env)->GetObjectClass(env, listener_obj);
    l.on_event = (*env)->GetMethodID(env, lc, "onEvent", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;IID)V");
    if (!l.on_event) (*env)->ExceptionClear(env);
    (*env)->DeleteLocalRef(env, lc);
  }
  char *json = NULL;
  int rc = pmk_start(&cfg, on_event, &l, &json, NULL);
  if (k) (*env)->ReleaseStringUTFChars(env, kernels, k);
  jstring out = new_string(env, json);
  pmk_free(json);
  if (!out && !(*env)->ExceptionCheck(env)) throw_new(env, "java/lang/IllegalStateException", pmk_strerror(rc));
  return out;
}

JNIEXPORT void JNICALL Java_org_prismark_Prismark_nativeCancel(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  pmk_cancel();
}

/* A touch on the screen during a run: the modes that need an idle device skip themselves, as for desktop input. */
JNIEXPORT void JNICALL Java_org_prismark_Prismark_nativeNotifyInput(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  pmk_notify_input();
}

JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeCompare(JNIEnv *env, jclass cls, jstring a, jstring b,
                                                                   jstring profiles) {
  (void)cls;
  const char *sa = (*env)->GetStringUTFChars(env, a, NULL), *sb = (*env)->GetStringUTFChars(env, b, NULL);
  const char *sp = profiles ? (*env)->GetStringUTFChars(env, profiles, NULL) : NULL;
  char *json = NULL, *text = NULL;
  int rc = sa && sb ? pmk_compare(sa, sb, sp, &json, &text) : PMK_ERR_NOMEM;
  if (sa) (*env)->ReleaseStringUTFChars(env, a, sa);
  if (sb) (*env)->ReleaseStringUTFChars(env, b, sb);
  if (sp) (*env)->ReleaseStringUTFChars(env, profiles, sp);
  jstring out = rc == PMK_OK ? new_string(env, json) : NULL;
  if (rc != PMK_OK && !(*env)->ExceptionCheck(env))
    throw_new(env, "java/lang/IllegalArgumentException", text ? text : pmk_strerror(rc));
  pmk_free(json);
  pmk_free(text);
  return out;
}
