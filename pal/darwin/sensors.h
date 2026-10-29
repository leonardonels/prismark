/*
 * Clocks, CPU energy and die temperature on Apple silicon Macs (sensors.c). Every reading is unavailable
 * (0, -1 or NaN) on Intel Macs, on iOS, and on a macOS that no longer has the interfaces they come from.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#ifndef PMK_DARWIN_SENSORS_H
#define PMK_DARWIN_SENSORS_H

#include "pal.h"

/* Looks the interfaces up; fills the CPUs' max_khz/min_khz and caps.power. nperf: the perflevel0 CPUs. */
void dsens_init(pmk_machine *m, int nperf);
/* Average clock in kHz of the cores of one type (0 performance, 1 efficiency) since the previous reading,
   weighted by the time each core ran; 0 if unknown or the type did not run. */
int dsens_cpu_khz(int efficiency);
/* The core type of each hardware CPU number (what pthread_cpu_number_np returns): types[n] is 0 for a
   performance core, 1 for an efficiency core. Returns how many were found, 0 where the registry does not say. */
int dsens_cpu_types(int *types, int max);
/* Hottest SoC die sensor in degrees C; NaN if unknown. */
double dsens_temp_c(void);
/* CPU energy counter in microjoules and the time it was read (pal_now_ns clock); -1 if unavailable. */
int dsens_energy_uj(long long *uj, uint64_t *ns);

#endif
