/* Copyright 2026 The Prismark Authors. Apache-2.0. */
#include "stats.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*
 * Quickselect (Hoare partition). On return a[0..k) <= a[k] <= a[k+1..n),
 * which the median and quantile helpers rely on.
 */
static double select_kth(double *a, size_t n, size_t k) {
  ptrdiff_t lo = 0, hi = (ptrdiff_t)n - 1, kk = (ptrdiff_t)k;
  while (lo < hi) {
    double pivot = a[lo + (hi - lo) / 2];
    ptrdiff_t i = lo, j = hi;
    while (i <= j) {
      while (a[i] < pivot) i++;
      while (a[j] > pivot) j--;
      if (i <= j) {
        double t = a[i];
        a[i] = a[j];
        a[j] = t;
        i++;
        j--;
      }
    }
    if (kk <= j) hi = j;
    else if (kk >= i) lo = i;
    else break;
  }
  return a[kk];
}

static double min_from(const double *a, size_t from, size_t n) {
  double m = a[from];
  for (size_t i = from + 1; i < n; i++)
    if (a[i] < m) m = a[i];
  return m;
}

/* Type-7 quantile of a scratch array, reordering it. */
static double quantile_inplace(double *a, size_t n, double q) {
  if (n == 0) return NAN;
  if (q <= 0) q = 0;
  if (q >= 1) q = 1;
  double h = (double)(n - 1) * q;
  size_t lo = (size_t)floor(h);
  double frac = h - (double)lo;
  double v = select_kth(a, n, lo);
  if (frac > 0 && lo + 1 < n) v += frac * (min_from(a, lo + 1, n) - v);
  return v;
}

static double median_inplace(double *a, size_t n) { return quantile_inplace(a, n, 0.5); }

double pmk_quantile(const double *x, size_t n, double q) {
  if (n == 0) return NAN;
  double *a = malloc(n * sizeof *a);
  if (!a) return NAN;
  memcpy(a, x, n * sizeof *a);
  double v = quantile_inplace(a, n, q);
  free(a);
  return v;
}

double pmk_median(const double *x, size_t n) { return pmk_quantile(x, n, 0.5); }

double pmk_max(const double *x, size_t n) {
  if (n == 0) return NAN;
  double m = x[0];
  for (size_t i = 1; i < n; i++)
    if (x[i] > m) m = x[i];
  return m;
}

static int cmp_double(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

/*
 * One bootstrap resample of x and its median. block 1 draws every sample independently. Samples that follow one
 * another in time and drift together (throughput windows of a sustained run) are drawn in runs of `block`
 * consecutive samples from random starting points, wrapping around (circular moving-block bootstrap): the
 * resample keeps their short-range correlation, so the interval is not narrowed by treating them as independent.
 */
static double resampled_median(const double *x, size_t n, size_t block, double *tmp, pmk_rng *r) {
  if (block <= 1) {
    for (size_t i = 0; i < n; i++) tmp[i] = x[pmk_rng_below(r, n)];
  } else {
    for (size_t i = 0; i < n;) {
      size_t start = (size_t)pmk_rng_below(r, n);
      for (size_t k = 0; k < block && i < n; k++, i++) tmp[i] = x[(start + k) % n];
    }
  }
  return median_inplace(tmp, n);
}

size_t pmk_block_len(size_t n) { return n < PMK_BOOT_MIN_N ? 1 : (size_t)(cbrt((double)n) + 0.5); }

/* Turns B bootstrap replicates into a percentile interval around est. */
static pmk_ci percentile_ci(double est, double *reps, int B) {
  pmk_ci c = {est, NAN, NAN};
  qsort(reps, (size_t)B, sizeof *reps, cmp_double);
  /* reps is sorted, so selection inside quantile_inplace is a no-op scan. */
  c.lo = quantile_inplace(reps, (size_t)B, 0.025);
  c.hi = quantile_inplace(reps, (size_t)B, 0.975);
  return c;
}

pmk_ci pmk_boot_median_lincomb(const double *const *sets, const size_t *ns, const double *coef,
                               int k, int B, pmk_rng *r) {
  pmk_ci c = {0, NAN, NAN};
  size_t nmax = 0, nmin = (size_t)-1;
  for (int i = 0; i < k; i++) {
    if (ns[i] == 0) return (pmk_ci){NAN, NAN, NAN};
    c.est += coef[i] * pmk_median(sets[i], ns[i]);
    if (ns[i] > nmax) nmax = ns[i];
    if (ns[i] < nmin) nmin = ns[i];
  }
  if (nmin < PMK_BOOT_MIN_N) return c; /* too few samples for an interval: the estimate alone */
  double *tmp = malloc(nmax * sizeof *tmp);
  double *reps = malloc((size_t)B * sizeof *reps);
  if (tmp && reps && B > 1) {
    for (int b = 0; b < B; b++) {
      double v = 0;
      for (int i = 0; i < k; i++) v += coef[i] * resampled_median(sets[i], ns[i], 1, tmp, r);
      reps[b] = v;
    }
    c = percentile_ci(c.est, reps, B);
  }
  free(tmp);
  free(reps);
  return c;
}

pmk_ci pmk_boot_median_block(const double *x, size_t n, size_t block, int B, pmk_rng *r) {
  pmk_ci c = {NAN, NAN, NAN};
  if (n == 0) return c;
  c.est = pmk_median(x, n);
  if (n < PMK_BOOT_MIN_N) return c;
  double *tmp = malloc(n * sizeof *tmp);
  double *reps = malloc((size_t)B * sizeof *reps);
  if (tmp && reps && B > 1) {
    for (int b = 0; b < B; b++) reps[b] = resampled_median(x, n, block, tmp, r);
    c = percentile_ci(c.est, reps, B);
  }
  free(tmp);
  free(reps);
  return c;
}

pmk_ci pmk_boot_median(const double *x, size_t n, int B, pmk_rng *r) { return pmk_boot_median_block(x, n, 1, B, r); }

int pmk_boot_ratio_reps_block(const double *a, size_t na, size_t ba, const double *b, size_t nb, size_t bb, int B,
                              pmk_rng *r, double *reps) {
  if (na == 0 || nb == 0) return -1;
  size_t nmax = na > nb ? na : nb;
  double *tmp = malloc(nmax * sizeof *tmp);
  if (!tmp) return -1;
  for (int i = 0; i < B; i++) {
    double ma = resampled_median(a, na, ba, tmp, r);
    double mb = resampled_median(b, nb, bb, tmp, r);
    reps[i] = ma / mb;
  }
  free(tmp);
  return 0;
}

int pmk_boot_ratio_reps(const double *a, size_t na, const double *b, size_t nb, int B, pmk_rng *r, double *reps) {
  return pmk_boot_ratio_reps_block(a, na, 1, b, nb, 1, B, r, reps);
}

pmk_ci pmk_boot_median_ratio_block(const double *a, size_t na, size_t ba, const double *b, size_t nb, size_t bb,
                                   int B, pmk_rng *r) {
  pmk_ci c = {NAN, NAN, NAN};
  if (na == 0 || nb == 0) return c;
  c.est = pmk_median(a, na) / pmk_median(b, nb);
  if (na < PMK_BOOT_MIN_N || nb < PMK_BOOT_MIN_N) return c; /* too few samples for an interval */
  double *reps = malloc((size_t)B * sizeof *reps);
  if (reps && B > 1 && !pmk_boot_ratio_reps_block(a, na, ba, b, nb, bb, B, r, reps)) c = percentile_ci(c.est, reps, B);
  free(reps);
  return c;
}

pmk_ci pmk_boot_median_ratio(const double *a, size_t na, const double *b, size_t nb, int B,
                             pmk_rng *r) {
  return pmk_boot_median_ratio_block(a, na, 1, b, nb, 1, B, r);
}

double pmk_t975(int df) {
  static const double t[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228, 2.201, 2.179, 2.160,
                             2.145,  2.131, 2.120, 2.110, 2.101, 2.093, 2.086, 2.080, 2.074, 2.069, 2.064, 2.060, 2.056,
                             2.052,  2.048, 2.045, 2.042};
  if (df < 1) return INFINITY;
  if (df <= 30) return t[df - 1];
  return 1.960 + 2.4 / (double)df; /* within 0.005 of the exact value from df 30 on */
}

int pmk_session_logvar(const double *x, size_t n, const uint32_t *starts, int k, double *var, int *df) {
  double sum = 0, sum2 = 0;
  int m = 0;
  for (int i = 0; i < k; i++) {
    size_t a = starts[i], b = i + 1 < k ? starts[i + 1] : n;
    if (b > n) b = n;
    if (b <= a) continue;
    double med = pmk_median(x + a, b - a);
    if (!(med > 0)) continue;
    double l = log(med);
    sum += l;
    sum2 += l * l;
    m++;
  }
  if (m < 2) return -1;
  double v = (sum2 - sum * sum / m) / (m - 1);
  *var = v > 0 ? v : 0;
  *df = m - 1;
  return 0;
}

void pmk_moderate(double var, int df, double pooled, int pooled_df, double *mvar, int *mdf) {
  int d0 = pooled_df < PMK_SESS_PRIOR_DF ? pooled_df : PMK_SESS_PRIOR_DF;
  if (d0 <= 0) {
    *mvar = var;
    *mdf = df;
    return;
  }
  *mvar = ((double)d0 * pooled + (double)df * var) / (double)(d0 + df);
  *mdf = d0 + df;
}

pmk_ci pmk_ci_from_reps(double est, double *reps, int B) { return percentile_ci(est, reps, B); }
