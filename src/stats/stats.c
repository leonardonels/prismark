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

static double resampled_median(const double *x, size_t n, double *tmp, pmk_rng *r) {
  for (size_t i = 0; i < n; i++) tmp[i] = x[pmk_rng_below(r, n)];
  return median_inplace(tmp, n);
}

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
  size_t nmax = 0;
  for (int i = 0; i < k; i++) {
    if (ns[i] == 0) return (pmk_ci){NAN, NAN, NAN};
    c.est += coef[i] * pmk_median(sets[i], ns[i]);
    if (ns[i] > nmax) nmax = ns[i];
  }
  double *tmp = malloc(nmax * sizeof *tmp);
  double *reps = malloc((size_t)B * sizeof *reps);
  if (tmp && reps && B > 1) {
    for (int b = 0; b < B; b++) {
      double v = 0;
      for (int i = 0; i < k; i++) v += coef[i] * resampled_median(sets[i], ns[i], tmp, r);
      reps[b] = v;
    }
    c = percentile_ci(c.est, reps, B);
  }
  free(tmp);
  free(reps);
  return c;
}

pmk_ci pmk_boot_median(const double *x, size_t n, int B, pmk_rng *r) {
  const double *sets[1] = {x};
  size_t ns[1] = {n};
  double coef[1] = {1.0};
  return pmk_boot_median_lincomb(sets, ns, coef, 1, B, r);
}

pmk_ci pmk_boot_median_ratio(const double *a, size_t na, const double *b, size_t nb, int B,
                             pmk_rng *r) {
  pmk_ci c = {NAN, NAN, NAN};
  if (na == 0 || nb == 0) return c;
  c.est = pmk_median(a, na) / pmk_median(b, nb);
  size_t nmax = na > nb ? na : nb;
  double *tmp = malloc(nmax * sizeof *tmp);
  double *reps = malloc((size_t)B * sizeof *reps);
  if (tmp && reps && B > 1) {
    for (int i = 0; i < B; i++) {
      double ma = resampled_median(a, na, tmp, r);
      double mb = resampled_median(b, nb, tmp, r);
      reps[i] = ma / mb;
    }
    c = percentile_ci(c.est, reps, B);
  }
  free(tmp);
  free(reps);
  return c;
}

int pmk_boot_ratio_reps(const double *a, size_t na, const double *b, size_t nb, int B, pmk_rng *r, double *reps) {
  if (na == 0 || nb == 0) return -1;
  size_t nmax = na > nb ? na : nb;
  double *tmp = malloc(nmax * sizeof *tmp);
  if (!tmp) return -1;
  for (int i = 0; i < B; i++) {
    double ma = resampled_median(a, na, tmp, r);
    double mb = resampled_median(b, nb, tmp, r);
    reps[i] = ma / mb;
  }
  free(tmp);
  return 0;
}

pmk_ci pmk_ci_from_reps(double est, double *reps, int B) { return percentile_ci(est, reps, B); }
