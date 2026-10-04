/*
 * ISA-level detection for selecting the max-level kernel tier. On x86-64 the
 * levels follow the psABI microarchitecture levels and include the OS check
 * (XGETBV) that the vector register state is enabled.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include <string.h>

#include "pal.h"

#if defined(__x86_64__) || defined(_M_X64)

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
static void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
  int v[4];
  __cpuidex(v, (int)leaf, (int)sub);
  for (int i = 0; i < 4; i++) r[i] = (unsigned)v[i];
}
static unsigned long long xgetbv0(void) { return _xgetbv(0); }
#else
#include <cpuid.h>
static void cpuid(unsigned leaf, unsigned sub, unsigned r[4]) {
  r[0] = r[1] = r[2] = r[3] = 0;
  __cpuid_count(leaf, sub, r[0], r[1], r[2], r[3]);
}
static unsigned long long xgetbv0(void) {
  unsigned lo, hi;
  __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
  return ((unsigned long long)hi << 32) | lo;
}
#endif

#define BIT(r, n) (((r) >> (n)) & 1u)

int pal_isa_supported(const char *level) {
  if (!strcmp(level, "x86-64-v2")) return 1; /* the baseline the core itself is built for */
  unsigned l0[4], l1[4], l7[4], e1[4];
  cpuid(0, 0, l0);
  if (l0[0] < 7) return 0;
  cpuid(1, 0, l1);
  cpuid(7, 0, l7);
  cpuid(0x80000001u, 0, e1);
  int osxsave = BIT(l1[2], 27);
  unsigned long long xcr0 = osxsave ? xgetbv0() : 0;
  int os_avx = (xcr0 & 0x6) == 0x6;        /* XMM and YMM state */
  int os_avx512 = (xcr0 & 0xe6) == 0xe6;   /* plus opmask, ZMM_Hi256, Hi16_ZMM */
  int v3 = os_avx && BIT(l1[2], 28) /* avx */ && BIT(l7[1], 5) /* avx2 */ && BIT(l7[1], 3) /* bmi1 */ &&
           BIT(l7[1], 8) /* bmi2 */ && BIT(l1[2], 12) /* fma */ && BIT(l1[2], 29) /* f16c */ &&
           BIT(e1[2], 5) /* lzcnt */ && BIT(l1[2], 22) /* movbe */;
  if (!strcmp(level, "x86-64-v3")) return v3;
  int v4 = v3 && os_avx512 && BIT(l7[1], 16) /* avx512f */ && BIT(l7[1], 17) /* dq */ && BIT(l7[1], 28) /* cd */ &&
           BIT(l7[1], 30) /* bw */ && BIT(l7[1], 31) /* vl */;
  if (!strcmp(level, "x86-64-v4")) return v4;
  return 0;
}

#elif defined(__aarch64__) || defined(_M_ARM64)

#if defined(__linux__)
#include <sys/auxv.h>
#ifndef HWCAP_ASIMDDP
#define HWCAP_ASIMDDP (1 << 20)
#endif
#ifndef HWCAP_FPHP
#define HWCAP_FPHP (1 << 9)
#endif
#ifndef HWCAP_ASIMDHP
#define HWCAP_ASIMDHP (1 << 10)
#endif
#ifndef HWCAP2_SVE2
#define HWCAP2_SVE2 (1 << 1)
#endif
static int has_dotprod_fp16(void) {
  unsigned long h = getauxval(AT_HWCAP);
  return (h & HWCAP_ASIMDDP) && (h & HWCAP_FPHP) && (h & HWCAP_ASIMDHP);
}
static int has_sve2(void) { return (getauxval(AT_HWCAP2) & HWCAP2_SVE2) != 0; }
#elif defined(__APPLE__)
#include <sys/sysctl.h>
static int sysctl_flag(const char *name) {
  int v = 0;
  size_t n = sizeof v;
  return sysctlbyname(name, &v, &n, NULL, 0) == 0 && v;
}
static int has_dotprod_fp16(void) {
  return sysctl_flag("hw.optional.arm.FEAT_DotProd") && sysctl_flag("hw.optional.arm.FEAT_FP16");
}
static int has_sve2(void) { return 0; /* no Apple core implements SVE */ }
#elif defined(_WIN32)
#include <windows.h>
#ifndef PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE
#define PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE 43
#endif
#ifndef PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE
#define PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE 47
#endif
static int has_dotprod_fp16(void) {
  /* Every core with the dot-product extension that Windows supports also has FP16 arithmetic. */
  return IsProcessorFeaturePresent(PF_ARM_V82_DP_INSTRUCTIONS_AVAILABLE) != 0;
}
static int has_sve2(void) { return IsProcessorFeaturePresent(PF_ARM_SVE2_INSTRUCTIONS_AVAILABLE) != 0; }
#else
static int has_dotprod_fp16(void) { return 0; }
static int has_sve2(void) { return 0; }
#endif

int pal_isa_supported(const char *level) {
  if (!strcmp(level, "armv8.2-a")) return 1;
  if (!strcmp(level, "armv8.2-a+dotprod+fp16")) return has_dotprod_fp16();
  if (!strcmp(level, "armv9-a+sve2")) return has_sve2() && has_dotprod_fp16();
  return 0;
}

#else
int pal_isa_supported(const char *level) {
  (void)level;
  return 0;
}
#endif
