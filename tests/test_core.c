/* Unit tests for the statistics, the JSON writer and reader, the kernels and comparisons.
 * Copyright 2026 The Prismark Authors. Apache-2.0. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "jparse.h"
#include "json.h"
#include "kernels.h"
#include "prismark/prismark.h"
#include "stats.h"
#include "zstd.h"

long pmk_k5_parse_count_baseline(const char *text, size_t len);

static int failures;

#define CHECK(cond)                                                   \
  do {                                                                \
    if (!(cond)) {                                                    \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      failures++;                                                     \
    }                                                                 \
  } while (0)

#define NEAR(a, b, tol) CHECK(fabs((a) - (b)) <= (tol))

static int cmp(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;
  return (x > y) - (x < y);
}

static void test_quantiles(void) {
  double odd[] = {5, 1, 4, 2, 3};
  double even[] = {4, 1, 3, 2};
  NEAR(pmk_median(odd, 5), 3, 0);
  NEAR(pmk_median(even, 4), 2.5, 0);
  NEAR(pmk_quantile(odd, 5, 0), 1, 0);
  NEAR(pmk_quantile(odd, 5, 1), 5, 0);
  NEAR(pmk_quantile(odd, 5, 0.25), 2, 0);
  NEAR(pmk_quantile(even, 4, 0.5), 2.5, 0);
  NEAR(pmk_quantile(even, 4, 0.9), 3.7, 1e-12);
  CHECK(isnan(pmk_median(NULL, 0)));
  NEAR(pmk_max(odd, 5), 5, 0);
  CHECK(odd[0] == 5 && odd[1] == 1); /* input untouched */

  /* Duplicates and random data against a sorted reference. */
  pmk_rng r;
  pmk_rng_seed(&r, 42);
  for (int trial = 0; trial < 200; trial++) {
    size_t n = 1 + pmk_rng_below(&r, 300);
    double *x = malloc(n * sizeof *x), *s = malloc(n * sizeof *s);
    for (size_t i = 0; i < n; i++) x[i] = (double)pmk_rng_below(&r, trial % 2 ? 5 : 1000000);
    memcpy(s, x, n * sizeof *s);
    qsort(s, n, sizeof *s, cmp);
    double q = pmk_rng_unit(&r);
    double h = (double)(n - 1) * q;
    size_t lo = (size_t)floor(h);
    double ref = s[lo] + (lo + 1 < n ? (h - (double)lo) * (s[lo + 1] - s[lo]) : 0);
    NEAR(pmk_quantile(x, n, q), ref, 1e-9);
    double med = n % 2 ? s[n / 2] : (s[n / 2 - 1] + s[n / 2]) / 2;
    NEAR(pmk_median(x, n), med, 1e-9);
    free(x);
    free(s);
  }
}

static void test_bootstrap(void) {
  pmk_rng r;
  pmk_rng_seed(&r, 7);
  enum { N = 400 };
  double a[N], b[N];
  for (int i = 0; i < N; i++) {
    a[i] = 100 + 10 * (pmk_rng_unit(&r) - 0.5);
    b[i] = 200 + 10 * (pmk_rng_unit(&r) - 0.5);
  }
  pmk_ci m = pmk_boot_median(a, N, PMK_BOOT_B, &r);
  CHECK(m.lo <= m.est && m.est <= m.hi);
  CHECK(m.lo > 98 && m.hi < 102);
  pmk_ci ratio = pmk_boot_median_ratio(a, N, b, N, PMK_BOOT_B, &r);
  CHECK(ratio.lo < 0.5 && ratio.hi > 0.5);
  CHECK(ratio.hi - ratio.lo < 0.03);
  const double *sets[2] = {b, a};
  size_t ns[2] = {N, N};
  double coef[2] = {1, -1};
  pmk_ci d = pmk_boot_median_lincomb(sets, ns, coef, 2, PMK_BOOT_B, &r);
  CHECK(d.lo < 100 && d.hi > 100 && pmk_ci_excludes_zero(d));
  /* A constant sample has a zero-width interval. */
  double c[10];
  for (int i = 0; i < 10; i++) c[i] = 3;
  pmk_ci z = pmk_boot_median(c, 10, 200, &r);
  CHECK(z.est == 3 && z.lo == 3 && z.hi == 3);
}

static void test_json(void) {
  pmk_jw w = {0};
  jw_obj_begin(&w, NULL);
  jw_str(&w, "s", "a\"b\\c\n\x01");
  jw_num(&w, "x", 1.5);
  jw_num(&w, "nan", NAN);
  jw_int(&w, "i", -42);
  jw_arr_begin(&w, "a");
  jw_int(&w, NULL, 1);
  jw_bool(&w, NULL, 1);
  jw_null(&w, NULL);
  jw_obj_begin(&w, NULL);
  jw_obj_end(&w);
  jw_arr_end(&w);
  jw_str(&w, "n", NULL);
  jw_obj_end(&w);
  char *s = buf_take(&w.b);
  const char *want = "{\"s\":\"a\\\"b\\\\c\\n\\u0001\",\"x\":1.5,\"nan\":null,\"i\":-42,\"a\":[1,true,null,{}],\"n\":null}";
  CHECK(s && !strcmp(s, want));
  if (s && strcmp(s, want)) fprintf(stderr, "got  %s\nwant %s\n", s, want);
  free(s);
}

static void test_kernels(void) {
  const pmk_kernels *k = &pmk_kernels_baseline;
  CHECK(k->k9_run(1000, 5) == 5 + 1000ull * k->k9_adds_per_iter);
  enum { N = 1000 };
  pmk_k7_node *nodes = aligned_alloc(64, N * sizeof *nodes);
  k->k7_build(nodes, N, 1);
  /* Sattolo: one cycle through every node. */
  unsigned char seen[N] = {0};
  uint64_t p = 0;
  for (int i = 0; i < N; i++) {
    CHECK(!seen[p]);
    seen[p] = 1;
    p = nodes[p].next;
  }
  CHECK(p == 0);
  CHECK(k->k7_chase(nodes, 0, N) == 0);
  free(nodes);
}


static void test_steady(void) {
  /* Exact exponential: tau recovered within the grid resolution (about 7 %). */
  enum { N = 300 };
  double t[N], y[N];
  for (int i = 0; i < N; i++) {
    t[i] = i + 1;
    y[i] = 80 - 40 * exp(-t[i] / 45.0);
  }
  double tinf;
  double tau = pmk_fit_tau(t, y, N, &tinf);
  CHECK(fabs(tau - 45) / 45 < 0.08);
  CHECK(fabs(tinf - 80) < 1.0);
  /* Flat temperature: no transient, tau reported as 0. */
  for (int i = 0; i < N; i++) y[i] = 45 + 0.1 * (i % 3);
  CHECK(pmk_fit_tau(t, y, N, NULL) == 0);
  CHECK(isnan(pmk_fit_tau(t, y, 3, NULL)));

  double flat[10] = {10, 10.1, 9.9, 10, 10.05, 9.95, 10, 10.1, 9.9, 10};
  double rising[10] = {10, 10.5, 11, 11.4, 12, 12.6, 13, 13.5, 14, 14.6};
  double rel;
  CHECK(pmk_slope_flat(flat, 10, &rel));
  CHECK(!pmk_slope_flat(rising, 10, &rel) && rel > 0);
  double constant[10] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
  CHECK(pmk_slope_flat(constant, 10, NULL));
}

static void test_jparse(void) {
  char err[128];
  jdoc *d = jdoc_parse("{\"a\": [1, 2.5, -3e2], \"s\": \"x\\u00e9\\n\", \"o\": {\"t\": true, \"n\": null}}", err,
                       sizeof err);
  CHECK(d != NULL);
  if (!d) return;
  const jv *r = jdoc_root(d);
  CHECK(jv_len(jv_get(r, "a")) == 3);
  NEAR(jv_num(&jv_get(r, "a")->items[2], 0), -300, 0);
  CHECK(!strcmp(jv_str(jv_get(r, "s"), ""), "x\xc3\xa9\n"));
  CHECK(jv_bool(jv_path(r, "o.t"), 0) == 1);
  CHECK(jv_path(r, "o.n")->t == JV_NULL);
  CHECK(jv_path(r, "o.missing") == NULL);
  jdoc_free(d);
  CHECK(jdoc_parse("{\"a\": [1, 2,]}", err, sizeof err) == NULL && err[0]);
  CHECK(jdoc_parse("[1] x", err, sizeof err) == NULL);
}

static void test_k5_parser(void) {
  const char *ok[] = {"{}", "[]", "[1,-2,3.5e-3,0,\"a\\\"b\",true,false,null]", "{\"k\":{\"x\":[[],{}]}}",
                      "\"\\ud83d\\ude00\"", " 42 "};
  const char *bad[] = {"[1,]", "{\"a\" 1}", "01", "[1 2]", "\"\\ud83d\"", "{\"a\":1,}", "tru", "[\"\x01\"]", ""};
  for (size_t i = 0; i < sizeof ok / sizeof *ok; i++) CHECK(pmk_k5_parse_count_baseline(ok[i], strlen(ok[i])) > 0);
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++)
    CHECK(pmk_k5_parse_count_baseline(bad[i], strlen(bad[i])) < 0);
  /* [1,2] -> ARR, INT, INT, END */
  CHECK(pmk_k5_parse_count_baseline("[1,2]", 5) == 4);
}

static void test_zstd_roundtrip(void) {
  const char *src = "prismark prismark prismark K3 round trip, round trip, round trip";
  size_t n = strlen(src), cap = ZSTD_compressBound(n);
  char *dst = malloc(cap), *back = malloc(n + 1);
  size_t c = ZSTD_compress(dst, cap, src, n, 3);
  CHECK(!ZSTD_isError(c));
  size_t d = ZSTD_decompress(back, n + 1, dst, c);
  CHECK(d == n && !memcmp(back, src, n));
  free(dst);
  free(back);
}

/* Every throughput kernel: burst inputs create, two jobs agree, and a fresh instance reproduces both hashes. */
static void test_throughput_kernels(void) {
  const pmk_kernels *k = &pmk_kernels_baseline;
  CHECK(k->ntk >= 7);
  for (size_t i = 0; i < k->ntk; i++) {
    const pmk_tk *tk = k->tk[i];
    if (!strcmp(tk->id, "K1")) continue; /* needs a snapshot */
    char err[128] = "";
    pmk_tk_args a = {PMK_SIZE_BURST, NULL, err, sizeof err};
    uint64_t in[2] = {0, 0}, out[2] = {0, 0};
    for (int rep = 0; rep < 2; rep++) {
      void *inst = tk->create(&a);
      CHECK(inst != NULL);
      if (!inst) break;
      void *sc = tk->scratch_new ? tk->scratch_new(inst) : NULL;
      in[rep] = tk->input_hash(inst);
      uint64_t h1 = 0, h2 = 0;
      size_t nt = tk->ntasks(inst);
      CHECK(nt > 0);
      for (size_t t = 0; t < nt; t++) {
        CHECK(tk->task_work(inst, t) > 0);
        h1 = pmk_job_combine(h1, t, tk->task(inst, sc, t));
      }
      for (size_t t = nt; t-- > 0;) h2 = pmk_job_combine(h2, t, tk->task(inst, sc, t)); /* any order */
      CHECK(h1 == h2 && h1 != 0);
      out[rep] = h1;
      if (sc && tk->scratch_free) tk->scratch_free(sc);
      tk->destroy(inst);
    }
    if (in[0] != in[1] || out[0] != out[1]) fprintf(stderr, "kernel %s not deterministic\n", tk->id);
    CHECK(in[0] == in[1] && out[0] == out[1]);
  }
}

/* A minimal result document with one windowed K2 series and one burst K3 series. */
static char *fake_result(double k2_perf, double k3_ns, int pinning) {
  pmk_jw w = {0};
  jw_obj_begin(&w, NULL);
  jw_str(&w, "schema", "prismark/1");
  jw_str(&w, "run_id", "test");
  jw_obj_begin(&w, "machine");
  jw_obj_begin(&w, "cpu");
  jw_str(&w, "model", "test cpu");
  jw_arr_begin(&w, "cores");
  jw_obj_begin(&w, NULL);
  jw_str(&w, "type", "P");
  jw_obj_end(&w);
  jw_arr_end(&w);
  jw_obj_end(&w);
  jw_obj_begin(&w, "capabilities");
  jw_bool(&w, "pinning", pinning);
  jw_bool(&w, "perf_counters", 0);
  jw_obj_end(&w);
  jw_obj_end(&w);
  jw_arr_begin(&w, "results");
  for (int n = 1; n <= 4; n *= 4) {
    jw_obj_begin(&w, NULL);
    jw_str(&w, "kernel", "K2");
    jw_str(&w, "mode", "mc_threaded");
    jw_str(&w, "tier", "baseline");
    jw_obj_begin(&w, "params");
    jw_str(&w, "size", "full");
    jw_int(&w, "threads", n);
    jw_num(&w, "window_ms", 1000);
    jw_obj_end(&w);
    jw_obj_begin(&w, "core");
    jw_str(&w, "requested", "P");
    jw_obj_end(&w);
    jw_str(&w, "unit", "Msamples/s");
    jw_arr_begin(&w, "samples");
    for (int i = 0; i < 30; i++) jw_num(&w, NULL, k2_perf * n * (1 + 0.001 * (i % 5)));
    jw_arr_end(&w);
    jw_obj_begin(&w, "steady");
    jw_int(&w, "from_window", 20);
    jw_obj_end(&w);
    jw_str(&w, "input_hash", "00000000000000aa");
    jw_obj_end(&w);
  }
  jw_obj_begin(&w, NULL);
  jw_str(&w, "kernel", "K3");
  jw_str(&w, "mode", "st_burst");
  jw_str(&w, "tier", "baseline");
  jw_obj_begin(&w, "params");
  jw_str(&w, "size", "burst");
  jw_obj_end(&w);
  jw_obj_begin(&w, "core");
  jw_str(&w, "requested", "P");
  jw_obj_end(&w);
  jw_str(&w, "unit", "ns");
  jw_arr_begin(&w, "samples");
  for (int i = 0; i < 40; i++) jw_num(&w, NULL, k3_ns * (1 + 0.002 * (i % 7)));
  jw_arr_end(&w);
  jw_str(&w, "input_hash", "00000000000000bb");
  jw_obj_end(&w);
  jw_arr_end(&w);
  jw_obj_end(&w);
  return buf_take(&w.b);
}

static void test_compare(void) {
  char *a = fake_result(2.0, 10e6, 1), *b = fake_result(1.0, 20e6, 1), *c = fake_result(1.0, 20e6, 0);
  char *json = NULL, *text = NULL;
  int rc = pmk_compare(a, b, NULL, &json, &text);
  CHECK(rc == PMK_OK && json && text);
  jdoc *d = json ? jdoc_parse(json, NULL, 0) : NULL;
  CHECK(d != NULL);
  if (d) {
    const jv *kernels = jv_get(jdoc_root(d), "kernels");
    CHECK(jv_len(kernels) == 3);
    for (size_t i = 0; i < jv_len(kernels); i++) {
      /* A is twice as fast everywhere: K2 throughput doubles, K3 time halves. */
      NEAR(jv_num(jv_path(&kernels->items[i], "r.est"), 0), 2.0, 0.01);
      CHECK(jv_bool(jv_get(&kernels->items[i], "different"), 0));
    }
    const jv *profiles = jv_get(jdoc_root(d), "profiles");
    CHECK(jv_len(profiles) == 4);
    for (size_t i = 0; i < jv_len(profiles); i++) {
      const jv *p = &profiles->items[i];
      const char *name = jv_str(jv_get(p, "name"), "");
      if (!strcmp(name, "Render")) { /* K2 at each machine's largest n */
        CHECK(!strcmp(jv_str(jv_get(p, "status"), ""), "ok"));
        NEAR(jv_num(jv_path(p, "H.est"), 0), 2.0, 0.01);
      } else if (!strcmp(name, "Daily") || !strcmp(name, "Dev")) { /* missing kernels: not reported */
        CHECK(!strncmp(jv_str(jv_get(p, "status"), ""), "not reported", 12));
      }
    }
    jdoc_free(d);
  }
  pmk_free(json);
  pmk_free(text);

  /* Custom profile: H is the weighted harmonic mean, sigma 0 when every r is equal. */
  const char *prof = "{\"profiles\":[{\"name\":\"T\",\"items\":[{\"kernel\":\"K3\",\"mode\":\"st_burst\",\"alpha\":0.5},"
                     "{\"kernel\":\"K2\",\"mode\":\"mc_threaded\",\"n\":1,\"alpha\":0.5}]}]}";
  rc = pmk_compare(a, b, prof, &json, &text);
  CHECK(rc == PMK_OK);
  d = json ? jdoc_parse(json, NULL, 0) : NULL;
  if (d) {
    const jv *p = &jv_get(jdoc_root(d), "profiles")->items[0];
    NEAR(jv_num(jv_path(p, "H.est"), 0), 2.0, 0.01);
    NEAR(jv_num(jv_path(p, "sigma_ln_r.est"), 1), 0.0, 0.01);
    jdoc_free(d);
  }
  pmk_free(json);
  pmk_free(text);

  /* Different capabilities: refused. */
  rc = pmk_compare(a, c, NULL, &json, &text);
  CHECK(rc == PMK_ERR_INVALID && json == NULL && text && strstr(text, "pinning"));
  pmk_free(text);

  /* Files from before ABI 4: the old root-only capabilities are ignored, and series measured with changed
   * power settings (the removed cold-burst grid) are left out, so only the two K2 series still match. */
  char *old = malloc(strlen(b) + 64);
  CHECK(old != NULL);
  if (old) {
    const char *caps = strstr(b, "\"capabilities\":{"), *k3 = strstr(b, "\"size\":\"burst\"");
    CHECK(caps && k3);
    if (caps && k3) {
      size_t at = (size_t)(caps - b) + strlen("\"capabilities\":{");
      size_t at2 = (size_t)(k3 - b);
      snprintf(old, strlen(b) + 64, "%.*s\"freq_control\":true,%.*s\"grid_cell\":\"idle_off/freq_pin\",%s", (int)at, b,
               (int)(at2 - at), b + at, b + at2);
      json = text = NULL;
      rc = pmk_compare(a, old, NULL, &json, &text);
      CHECK(rc == PMK_OK && json);
      d = json ? jdoc_parse(json, NULL, 0) : NULL;
      CHECK(d && jv_len(jv_get(jdoc_root(d), "kernels")) == 2);
      if (d) jdoc_free(d);
      pmk_free(json);
      pmk_free(text);
    }
    free(old);
  }
  free(a);
  free(b);
  free(c);
}

int main(void) {
  test_quantiles();
  test_bootstrap();
  test_json();
  test_kernels();
  test_steady();
  test_jparse();
  test_k5_parser();
  test_zstd_roundtrip();
  test_throughput_kernels();
  test_compare();
  if (failures) {
    fprintf(stderr, "%d check(s) failed\n", failures);
    return 1;
  }
  puts("all tests passed");
  return 0;
}
