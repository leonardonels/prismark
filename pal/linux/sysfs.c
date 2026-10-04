/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#define _GNU_SOURCE
#include "sysfs.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int sysfs_read(const char *path, char *out, size_t n) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  ssize_t r = read(fd, out, n - 1);
  close(fd);
  if (r < 0) return -1;
  out[r] = 0;
  while (r > 0 && (out[r - 1] == '\n' || out[r - 1] == ' ' || out[r - 1] == 0)) out[--r] = 0;
  return 0;
}

long long sysfs_read_ll(const char *path, long long def) {
  char b[64], *e;
  if (sysfs_read(path, b, sizeof b)) return def;
  long long v = strtoll(b, &e, 10);
  return e == b ? def : v;
}

int sysfs_write(const char *path, const char *v) {
  int fd = open(path, O_WRONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  size_t n = strlen(v);
  ssize_t w = write(fd, v, n);
  close(fd);
  return w == (ssize_t)n ? 0 : -1;
}

int sysfs_write_ll(const char *path, long long v) {
  char b[32];
  snprintf(b, sizeof b, "%lld", v);
  return sysfs_write(path, b);
}

void sysfs_parse_cpulist(const char *s, unsigned char *mask, int max) {
  while (*s) {
    char *e;
    long a = strtol(s, &e, 10);
    if (e == s) break;
    long b = a;
    s = e;
    if (*s == '-') {
      b = strtol(s + 1, &e, 10);
      s = e;
    }
    for (long i = a; i <= b && i < max; i++)
      if (i >= 0) mask[i] = 1;
    while (*s == ',' || *s == ' ' || *s == '\n') s++;
  }
}

unsigned char *sysfs_online_mask(int *nconf) {
  int n = (int)sysconf(_SC_NPROCESSORS_CONF);
  if (n <= 0) return NULL;
  unsigned char *mask = calloc((size_t)n, 1);
  if (!mask) return NULL;
  char list[4096];
  if (!sysfs_read(SYS_CPU "/online", list, sizeof list)) sysfs_parse_cpulist(list, mask, n);
  else memset(mask, 1, (size_t)n);
  *nconf = n;
  return mask;
}

int sysfs_idle_state_count(int cpu) {
  char p[128];
  int n = 0;
  for (; n < SYSFS_MAX_IDLE_STATES; n++) {
    snprintf(p, sizeof p, SYS_CPU "/cpu%d/cpuidle/state%d/name", cpu, n);
    if (access(p, R_OK)) break;
  }
  return n;
}
