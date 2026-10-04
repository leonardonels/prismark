/*
 * K9 — calibrated L1 loop. A dependent chain of integer adds: each add needs
 * the previous result, so an iteration costs about K9_ADDS cycles on a core
 * with 1-cycle add latency. The adds are written as inline assembly, one
 * instruction per add, so the compiler can neither fold the chain into a
 * multiply nor choose a different instruction. The cycles per iteration are
 * measured, not assumed (see the engine's K9 calibration).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include "kernels.h"

#if defined(__x86_64__)
#define ADD1 __asm__ volatile("add $1, %0" : "+r"(x));
#elif defined(__aarch64__)
#define ADD1 __asm__ volatile("add %0, %0, #1" : "+r"(x));
#else
#error "K9 needs an add instruction for this ISA"
#endif
#define ADD8 ADD1 ADD1 ADD1 ADD1 ADD1 ADD1 ADD1 ADD1
#define ADD64 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8 ADD8

uint64_t PMK_SYM(pmk_k9_run)(uint64_t iters, uint64_t x) {
  for (uint64_t i = 0; i < iters; i++) {
    ADD64
  }
  return x;
}
