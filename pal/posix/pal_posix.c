/*
 * Parts of the platform layer shared by every POSIX system (Linux, Android,
 * macOS, iOS): threads, locks, aligned memory, tier modules, child processes.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pal.h"

/* ---------- threads ---------- */

struct pal_thread {
  pthread_t th;
};

pal_thread *pal_thread_start(void *(*fn)(void *), void *arg) {
  pal_thread *t = malloc(sizeof *t);
  if (!t) return NULL;
  if (pthread_create(&t->th, NULL, fn, arg)) {
    free(t);
    return NULL;
  }
  return t;
}

void pal_thread_join(pal_thread *t) {
  if (!t) return;
  pthread_join(t->th, NULL);
  free(t);
}

struct pal_lock {
  pthread_mutex_t m;
  pthread_cond_t c;
};

pal_lock *pal_lock_new(void) {
  pal_lock *l = malloc(sizeof *l);
  if (!l) return NULL;
  pthread_mutex_init(&l->m, NULL);
  pthread_cond_init(&l->c, NULL);
  return l;
}

void pal_lock_free(pal_lock *l) {
  if (!l) return;
  pthread_cond_destroy(&l->c);
  pthread_mutex_destroy(&l->m);
  free(l);
}

void pal_lock_acquire(pal_lock *l) { pthread_mutex_lock(&l->m); }
void pal_lock_release(pal_lock *l) { pthread_mutex_unlock(&l->m); }
void pal_lock_wait(pal_lock *l) { pthread_cond_wait(&l->c, &l->m); }
void pal_lock_broadcast(pal_lock *l) { pthread_cond_broadcast(&l->c); }

void *pal_aligned_alloc(size_t align, size_t n) {
  void *p = NULL;
  return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, n ? n : 1) ? NULL : p;
}

void pal_aligned_free(void *p) { free(p); }

/* ---------- tier modules ---------- */

/* CMake names MODULE libraries *.so on Linux, Android and macOS alike; APKs only package lib*.so. */
#define MODULE_SUFFIX ".so"
#if defined(__ANDROID__)
#define MODULE_PREFIX "lib"
#else
#define MODULE_PREFIX ""
#endif

/* Directory holding the core: the executable, or the shared library the core is linked into. */
static void core_dir(char *out, size_t n) {
  const char *env = getenv("PRISMARK_KERNEL_DIR");
  if (env && *env) {
    snprintf(out, n, "%s", env);
    return;
  }
  Dl_info info;
  out[0] = 0;
  if (dladdr((void *)core_dir, &info) && info.dli_fname && strchr(info.dli_fname, '/')) {
    snprintf(out, n, "%s", info.dli_fname);
  } else {
#if defined(__linux__)
    ssize_t len = readlink("/proc/self/exe", out, n - 1);
    out[len > 0 ? len : 0] = 0;
#endif
  }
  char *slash = strrchr(out, '/');
  if (slash) *slash = 0;
  else snprintf(out, n, ".");
}

const pmk_kernels *pal_load_tier(const char *tier, char *err, size_t errlen) {
  char dir[1024], path[1200];
  core_dir(dir, sizeof dir);
  snprintf(path, sizeof path, "%s/" MODULE_PREFIX "prismark-kernels-%s" MODULE_SUFFIX, dir, tier);
  void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    const char *e = dlerror();
    snprintf(err, errlen, "cannot load %s: %s", path, e ? e : "unknown error");
    return NULL;
  }
  pmk_kernels_entry_fn entry = (pmk_kernels_entry_fn)dlsym(h, PMK_KERNELS_ENTRY);
  const pmk_kernels *k = entry ? entry() : NULL;
  if (!k || k->abi != PMK_KERNELS_ABI) {
    snprintf(err, errlen, "%s: %s", path, k ? "kernel ABI mismatch" : "no entry point");
    dlclose(h);
    return NULL;
  }
  return k;
}

/* ---------- processes ---------- */

int pal_run(const char *const *argv, const char *cwd, const char *log_path, int (*cancelled)(void)) {
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (cwd && chdir(cwd)) _exit(127);
    if (log_path) {
      int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
      if (fd >= 0) {
        dup2(fd, 1);
        dup2(fd, 2);
      }
    }
    int null = open("/dev/null", O_RDONLY);
    if (null >= 0) dup2(null, 0);
    execvp(argv[0], (char *const *)argv);
    _exit(127);
  }
  int status = 0, killed = 0;
  for (;;) {
    pid_t r = waitpid(pid, &status, cancelled ? WNOHANG : 0);
    if (r == pid) break;
    if (r < 0 && errno != EINTR) return -1;
    if (r == 0) {
      if (!killed && cancelled()) {
        kill(pid, SIGTERM);
        killed = 1;
      }
      pal_sleep_ns(50000000);
    }
  }
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int pal_copy_tree(const char *src, const char *dst) {
  const char *argv[] = {"cp", "-R", src, dst, NULL};
  return pal_run(argv, NULL, NULL, NULL) == 0 ? 0 : -1;
}

int pal_remove_tree(const char *path) {
  if (!path || !*path || !strcmp(path, "/")) return -1;
  const char *argv[] = {"rm", "-rf", path, NULL};
  return pal_run(argv, NULL, NULL, NULL) == 0 ? 0 : -1;
}
