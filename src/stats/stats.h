/* Robust statistics: medians, quantiles, bootstrap confidence intervals.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_STATS_H
#define PMK_STATS_H

#include <stddef.h>

#include "rng.h"

#define PMK_BOOT_B 2000 /* resamples for reported intervals */

typedef struct pmk_ci {
  double est, lo, hi; /* point estimate and 95% percentile-bootstrap interval */
} pmk_ci;

/* Median of x[0..n). NaN when n == 0. Does not modify x. */
double pmk_median(const double *x, size_t n);

/* Quantile q in [0,1] with linear interpolation (Hyndman-Fan type 7). */
double pmk_quantile(const double *x, size_t n, double q);

double pmk_max(const double *x, size_t n);

/* Bootstrap CI of the median. */
pmk_ci pmk_boot_median(const double *x, size_t n, int B, pmk_rng *r);

/* Bootstrap CI of median(a) / median(b), resampling a and b independently. */
pmk_ci pmk_boot_median_ratio(const double *a, size_t na, const double *b, size_t nb, int B,
                             pmk_rng *r);

/* B bootstrap replicates of median(a) / median(b) into reps[0..B). 0 on success. */
int pmk_boot_ratio_reps(const double *a, size_t na, const double *b, size_t nb, int B, pmk_rng *r, double *reps);

/* 95% percentile interval of B replicates around est (reorders reps). */
pmk_ci pmk_ci_from_reps(double est, double *reps, int B);

/* Bootstrap CI of sum_i coef[i] * median(set_i), resampling each set independently. */
pmk_ci pmk_boot_median_lincomb(const double *const *sets, const size_t *ns, const double *coef,
                               int k, int B, pmk_rng *r);

/*
 * Thermal time constant: fits T(t) = T_inf + (T_0 - T_inf) exp(-t / tau) by a
 * log-spaced grid search over tau with linear least squares for T_inf and
 * T_0 at each tau. Returns tau (in the units of t), or NaN when the samples
 * are too few, non-finite or show no transient. *t_inf may be NULL.
 */
double pmk_fit_tau(const double *t, const double *y, size_t n, double *t_inf);

/*
 * Slope test over y[0..n) at unit spacing: returns 1 when the least-squares
 * slope is not significantly different from zero (|t| < 2.0, about 95 %).
 * *rel_slope (may be NULL) receives the slope relative to the mean.
 */
int pmk_slope_flat(const double *y, size_t n, double *rel_slope);

/* True when the CI excludes zero. */
static inline int pmk_ci_excludes_zero(pmk_ci c) { return c.lo > 0 || c.hi < 0; }

#endif
