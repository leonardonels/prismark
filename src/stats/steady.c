/* Steady-state detection (spec 9.3). Copyright 2026 The Prismark Authors. Apache-2.0. */
#include <math.h>

#include "stats.h"

/* Least squares of y on (1, x): returns the residual sum of squares, coefficients in *a, *b. */
static double linfit(const double *x, const double *y, size_t n, double *a, double *b) {
  double sx = 0, sy = 0, sxx = 0, sxy = 0;
  for (size_t i = 0; i < n; i++) {
    sx += x[i];
    sy += y[i];
    sxx += x[i] * x[i];
    sxy += x[i] * y[i];
  }
  double d = (double)n * sxx - sx * sx;
  if (fabs(d) < 1e-300) {
    *a = sy / (double)n;
    *b = 0;
  } else {
    *b = ((double)n * sxy - sx * sy) / d;
    *a = (sy - *b * sx) / (double)n;
  }
  double rss = 0;
  for (size_t i = 0; i < n; i++) {
    double e = y[i] - (*a + *b * x[i]);
    rss += e * e;
  }
  return rss;
}

double pmk_fit_tau(const double *t, const double *y, size_t n, double *t_inf) {
  enum { GRID = 120, MAXN = 4096 };
  if (t_inf) *t_inf = NAN;
  if (n < 5) return NAN;
  /* Long runs are decimated to MAXN points; the fit does not need more. */
  size_t step = (n + MAXN - 1) / MAXN, m = 0;
  double xs[MAXN], ys[MAXN], e[MAXN];
  for (size_t i = 0; i < n && m < MAXN; i += step) {
    if (!isfinite(t[i]) || !isfinite(y[i])) return NAN;
    xs[m] = t[i];
    ys[m] = y[i];
    m++;
  }
  double span = xs[m - 1] - xs[0];
  if (!(span > 0)) return NAN;
  double lo = 0.02 * span, hi = 50.0 * span;
  double best_rss = INFINITY, best_tau = NAN, best_a = NAN, best_b = 0;
  for (int g = 0; g < GRID; g++) {
    double tau = lo * pow(hi / lo, (double)g / (GRID - 1));
    /* y = T_inf + (T_0 - T_inf) e,  e = exp(-(t - t0) / tau): linear in (T_inf, T_0 - T_inf). */
    for (size_t i = 0; i < m; i++) e[i] = exp(-(xs[i] - xs[0]) / tau);
    double a, b;
    double rss = linfit(e, ys, m, &a, &b);
    if (rss < best_rss) {
      best_rss = rss;
      best_tau = tau;
      best_a = a;
      best_b = b;
    }
  }
  /* No transient (flat temperature) or no decay within the grid: tau is not identifiable. */
  double ymin = ys[0], ymax = ys[0];
  for (size_t i = 1; i < m; i++) {
    ymin = fmin(ymin, ys[i]);
    ymax = fmax(ymax, ys[i]);
  }
  if (ymax - ymin < 0.5 || fabs(best_b) < 0.25) {
    if (t_inf) *t_inf = ymax - ymin < 0.5 ? (ymin + ymax) / 2 : NAN;
    return ymax - ymin < 0.5 ? 0.0 : NAN;
  }
  if (t_inf) *t_inf = best_a;
  return best_tau;
}

int pmk_slope_flat(const double *y, size_t n, double *rel_slope) {
  if (rel_slope) *rel_slope = NAN;
  if (n < 3) return 0;
  double x[4096];
  if (n > 4096) {
    y += n - 4096;
    n = 4096;
  }
  double mean = 0;
  for (size_t i = 0; i < n; i++) {
    x[i] = (double)i;
    mean += y[i];
  }
  mean /= (double)n;
  double a, b;
  double rss = linfit(x, y, n, &a, &b);
  double sxx = 0, xm = ((double)n - 1) / 2;
  for (size_t i = 0; i < n; i++) sxx += (x[i] - xm) * (x[i] - xm);
  double se = sqrt(rss / ((double)n - 2) / sxx);
  if (rel_slope && mean != 0) *rel_slope = b / mean;
  if (se == 0) return b == 0;
  return fabs(b / se) < 2.0;
}
