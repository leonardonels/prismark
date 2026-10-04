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
#include <stdlib.h>
#include <string.h>

#include "prismark/prismark.h"

typedef struct listener {
  JNIEnv *env;
  jobject obj;
  jmethodID on_event;
} listener;

static jstring new_string(JNIEnv *env, const char *s) { return s ? (*env)->NewStringUTF(env, s) : NULL; }

static void on_event(const pmk_event *ev, void *user) {
  listener *l = user;
  if (!l->obj || !l->on_event) return;
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

/* Returns the result document (also for partial runs), or throws with the error. */
JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeRun(JNIEnv *env, jclass cls, jint modes, jstring kernels,
                                                               jboolean quick, jobject listener_obj) {
  (void)cls;
  pmk_config cfg;
  pmk_config_init(&cfg);
  cfg.modes = (uint32_t)modes;
  cfg.frontend = "gui";
  cfg.ui_state = "frozen";
  const char *k = kernels ? (*env)->GetStringUTFChars(env, kernels, NULL) : NULL;
  cfg.kernels = k;
  if (quick) {
    cfg.max_reps = 20;
    cfg.cold_max_reps = 20;
    cfg.periodic_seconds = 10;
    cfg.window_ms = 250;
    cfg.sustained_min_s = 3;
    cfg.sustained_max_s = 6;
  }
  listener l = {env, listener_obj, NULL};
  if (listener_obj) {
    jclass lc = (*env)->GetObjectClass(env, listener_obj);
    l.on_event = (*env)->GetMethodID(env, lc, "onEvent", "(ILjava/lang/String;Ljava/lang/String;Ljava/lang/String;IID)V");
    if (!l.on_event) (*env)->ExceptionClear(env);
  }
  char *json = NULL;
  int rc = pmk_start(&cfg, on_event, &l, &json, NULL);
  if (k) (*env)->ReleaseStringUTFChars(env, kernels, k);
  jstring out = new_string(env, json);
  pmk_free(json);
  if (!out) {
    jclass ex = (*env)->FindClass(env, "java/lang/IllegalStateException");
    (*env)->ThrowNew(env, ex, pmk_strerror(rc));
  }
  return out;
}

JNIEXPORT void JNICALL Java_org_prismark_Prismark_nativeCancel(JNIEnv *env, jclass cls) {
  (void)env;
  (void)cls;
  pmk_cancel();
}

JNIEXPORT jstring JNICALL Java_org_prismark_Prismark_nativeCompare(JNIEnv *env, jclass cls, jstring a, jstring b,
                                                                   jstring profiles) {
  (void)cls;
  const char *sa = (*env)->GetStringUTFChars(env, a, NULL), *sb = (*env)->GetStringUTFChars(env, b, NULL);
  const char *sp = profiles ? (*env)->GetStringUTFChars(env, profiles, NULL) : NULL;
  char *json = NULL, *text = NULL;
  int rc = pmk_compare(sa, sb, sp, &json, &text);
  (*env)->ReleaseStringUTFChars(env, a, sa);
  (*env)->ReleaseStringUTFChars(env, b, sb);
  if (sp) (*env)->ReleaseStringUTFChars(env, profiles, sp);
  jstring out = rc == PMK_OK ? new_string(env, json) : NULL;
  if (rc != PMK_OK) {
    jclass ex = (*env)->FindClass(env, "java/lang/IllegalArgumentException");
    (*env)->ThrowNew(env, ex, text ? text : pmk_strerror(rc));
  }
  pmk_free(json);
  pmk_free(text);
  return out;
}
