/* Robust statistics: medians, quantiles, bootstrap confidence intervals.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#ifndef PMK_STATS_H
#define PMK_STATS_H

#include <stddef.h>
#include <stdint.h>

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

/* Fewer samples than this give an estimate without an interval (lo, hi NaN): a percentile bootstrap of a
   handful of values only returns their extremes. */
#define PMK_BOOT_MIN_N 8

/* Block length for samples correlated in time (throughput windows): about n^(1/3), 1 below PMK_BOOT_MIN_N. */
size_t pmk_block_len(size_t n);
/* The bootstrap functions below with a block length per sample set: block 1 resamples independently, a longer
   block keeps runs of consecutive samples together (circular moving-block bootstrap). */
pmk_ci pmk_boot_median_block(const double *x, size_t n, size_t block, int B, pmk_rng *r);
pmk_ci pmk_boot_median_ratio_block(const double *a, size_t na, size_t ba, const double *b, size_t nb, size_t bb,
                                   int B, pmk_rng *r);
int pmk_boot_ratio_reps_block(const double *a, size_t na, size_t ba, const double *b, size_t nb, size_t bb, int B,
                              pmk_rng *r, double *reps);

/* Bootstrap CI of the median. */
pmk_ci pmk_boot_median(const double *x, size_t n, int B, pmk_rng *r);

/* Bootstrap CI of median(a) / median(b), resampling a and b independently. */
pmk_ci pmk_boot_median_ratio(const double *a, size_t na, const double *b, size_t nb, int B,
                             pmk_rng *r);

/* B bootstrap replicates of median(a) / median(b) into reps[0..B). 0 on success. */
int pmk_boot_ratio_reps(const double *a, size_t na, const double *b, size_t nb, int B, pmk_rng *r, double *reps);

/*
 * Intervals from sessions. A series measured in k sessions at separate moments of a run has k session medians;
 * the variance of their logarithms is the between-session variance of the measurement, which an interval over
 * the repetitions of one sitting does not contain. The interval of the series is
 *     est * exp(+- t(df) * sqrt(var / k)).
 * Two or three sessions leave few degrees of freedom (t is 4.30 at df 2), so the variance is moderated with
 * that of the other series of the same mode, which were measured in the same rounds:
 *     var' = (d0 * pooled + df * var) / (d0 + df),   df' = d0 + df,   d0 = min(PMK_SESS_PRIOR_DF, pooled df).
 * A series as steady as its neighbours gets their common variance with more degrees of freedom; one that is
 * noisier keeps most of its own.
 */
#define PMK_SESS_PRIOR_DF 4
/* Two-sided 95 % quantile of Student's t; df < 1 gives infinity. */
double pmk_t975(int df);
/* Variance of ln(session median) over the k sessions of x[0..n) starting at starts[0..k) (ascending), with its
   degrees of freedom k - 1. Returns 0, or -1 when there are fewer than two usable sessions. */
int pmk_session_logvar(const double *x, size_t n, const uint32_t *starts, int k, double *var, int *df);
/* The moderated variance and its degrees of freedom (above); pooled_df 0 leaves var and df as they are. */
void pmk_moderate(double var, int df, double pooled, int pooled_df, double *mvar, int *mdf);

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
