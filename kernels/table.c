/* Per-tier kernel table. Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "kernels.h"

uint64_t PMK_SYM(pmk_k9_run)(uint64_t iters, uint64_t x);
void PMK_SYM(pmk_k7_build)(pmk_k7_node *nodes, size_t n, uint64_t seed);
uint64_t PMK_SYM(pmk_k7_chase)(const pmk_k7_node *nodes, uint64_t start, uint64_t steps);

extern const pmk_tk PMK_SYM(pmk_k2_tk);
extern const pmk_tk PMK_SYM(pmk_k3_tk);
extern const pmk_tk PMK_SYM(pmk_k4_tk);
extern const pmk_tk PMK_SYM(pmk_k5_tk);
extern const pmk_tk PMK_SYM(pmk_k6_tk);
extern const pmk_tk PMK_SYM(pmk_k8_fp32_tk);
extern const pmk_tk PMK_SYM(pmk_k8_int8_tk);
#ifdef PMK_HAVE_K1
extern const pmk_tk PMK_SYM(pmk_k1_tk);
#endif

#define PMK_STR2(x) #x
#define PMK_STR(x) PMK_STR2(x)

static const pmk_tk *const PMK_SYM(tk_list)[] = {
#ifdef PMK_HAVE_K1
    &PMK_SYM(pmk_k1_tk),
#endif
    &PMK_SYM(pmk_k2_tk),      &PMK_SYM(pmk_k3_tk),      &PMK_SYM(pmk_k4_tk),
    &PMK_SYM(pmk_k5_tk),      &PMK_SYM(pmk_k6_tk),      &PMK_SYM(pmk_k8_fp32_tk),
    &PMK_SYM(pmk_k8_int8_tk),
};

const pmk_kernels PMK_SYM(pmk_kernels) = {
    .abi = PMK_KERNELS_ABI,
    .tier = PMK_TIER_KIND,
    .level = PMK_TIER_LEVEL,
    .march = PMK_TIER_MARCH,
    .k9_adds_per_iter = 64,
    .k9_run = PMK_SYM(pmk_k9_run),
    .k7_build = PMK_SYM(pmk_k7_build),
    .k7_chase = PMK_SYM(pmk_k7_chase),
    .tk = PMK_SYM(tk_list),
    .ntk = sizeof PMK_SYM(tk_list) / sizeof *PMK_SYM(tk_list),
};

#ifdef PMK_TIER_MODULE
#if defined(_WIN32)
#define PMK_EXPORT __declspec(dllexport)
#else
#define PMK_EXPORT __attribute__((visibility("default")))
#endif
PMK_EXPORT const pmk_kernels *prismark_kernels_entry(void);
PMK_EXPORT const pmk_kernels *prismark_kernels_entry(void) { return &PMK_SYM(pmk_kernels); }
#endif
