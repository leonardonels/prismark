/* Small sysfs/procfs helpers for the Linux PAL.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_SYSFS_H
#define PMK_SYSFS_H

#include <stddef.h>

#define SYS_CPU "/sys/devices/system/cpu"
#define SYSFS_MAX_IDLE_STATES 16

/* Reads a file into out, trimming trailing whitespace. 0 on success. */
int sysfs_read(const char *path, char *out, size_t n);
long long sysfs_read_ll(const char *path, long long def);
int sysfs_write(const char *path, const char *v);
int sysfs_write_ll(const char *path, long long v);

/* Sets mask[i] for every CPU in a list such as "0-3,8-11". */
void sysfs_parse_cpulist(const char *s, unsigned char *mask, int max);
/* Online-CPU mask of length *nconf (configured CPUs); caller frees. NULL on error. */
unsigned char *sysfs_online_mask(int *nconf);
int sysfs_idle_state_count(int cpu);

#endif
