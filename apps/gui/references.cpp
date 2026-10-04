/*
 * Reference systems for the ranking.
 *
 * PLACEHOLDER VALUES. These are not measurements; they only give the ranking
 * something to compare against until real reference results exist. Every
 * entry is flagged `placeholder` and the app labels it as such.
 *
 * Real references replace them as result files: put a prismark-*.json from a
 * reference machine into the app's "references" folder (Help > Data folders).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
#include "metrics.h"

namespace {

Value m(double v, double rel = 0.01) { return Value{v, v * (1 - rel), v * (1 + rel)}; }
Value tail(double v) { return Value{v}; }

RunSummary ref(const char *id, const char *name, const char *os, const char *isa, int ncpu, const char *types,
               const char *max_level, bool mobile, std::initializer_list<std::pair<const char *, Value>> values) {
  RunSummary r;
  r.id = QStringLiteral("placeholder-") + id;
  r.name = name;
  r.reference = true;
  r.placeholder = true;
  r.machine = QJsonObject{{"os", os}, {"isa", isa}, {"cpus", ncpu}, {"types", types}};
  r.tiers = QJsonObject{{"baseline", QJsonObject{{"level", QString(isa) == "aarch64" ? "armv8.2-a" : "x86-64-v2"}}},
                        {"max", QJsonObject{{"level", max_level}}}};
  if (mobile)
    r.unavailable = QJsonArray{QJsonObject{{"kernel", "K1"}, {"mode", "all"}, {"reason", "desktop platforms only"}},
                               QJsonObject{{"kernel", "K1x"}, {"mode", "mc_threaded"}, {"reason", "desktop platforms only"}}};
  for (const auto &kv : values) r.metrics[kv.first] = kv.second;
  return r;
}

}  // namespace

QVector<RunSummary> placeholderReferences() {
  return {
      ref("desktop-16c", "Desktop x86-64, 16 cores / 32 threads", "linux", "x86_64", 32, "32×P", "x86-64-v4", false,
          {{"mc_k2", m(74, .02)}, {"mci_k2", m(79, .02)}, {"mc_k1", m(152, .02)}, {"mc_k1x", m(171, .02)},
           {"st_k2", m(4.3)}, {"st_k1", m(9.8)}, {"b_k3", m(10.6)}, {"b_k4", m(27.9)}, {"b_k5", m(13.1)},
           {"b_k6", m(9.7)}, {"r_1ms", m(64, .04)}, {"r_10ms", m(95)}, {"c_k4", m(30.8, .02)}, {"c_k6", m(11.1, .03)},
           {"m_lat1", m(76)}, {"m_latn", m(94)}, {"j_idle", tail(41)}, {"j_load", tail(155)}, {"u_k2", m(1.07)},
           {"u_k3", m(.98)}, {"u_k4", m(1.14)}, {"u_k8f", m(2.71)}, {"u_k8i", m(2.05)}}),
      ref("laptop-8c", "Laptop x86-64, 8 cores / 16 threads", "linux", "x86_64", 16, "16×P", "x86-64-v4", false,
          {{"mc_k2", m(38.5, .03)}, {"mci_k2", m(40.2, .03)}, {"mc_k1", m(78, .03)}, {"mc_k1x", m(342, .03)},
           {"st_k2", m(3.6)}, {"st_k1", m(8.1)}, {"b_k3", m(12.9)}, {"b_k4", m(34.5)}, {"b_k5", m(16.4)},
           {"b_k6", m(12.3)}, {"r_1ms", m(57, .05)}, {"r_10ms", m(93)}, {"c_k4", m(36.9, .03)}, {"c_k6", m(13.6, .03)},
           {"m_lat1", m(97)}, {"m_latn", m(108)}, {"j_idle", tail(260)}, {"j_load", tail(1880)}, {"u_k2", m(1.05)},
           {"u_k3", m(.96)}, {"u_k4", m(1.12)}, {"u_k8f", m(2.9)}, {"u_k8i", m(1.97)}}),
      ref("hybrid-6p8e", "Hybrid laptop x86-64, 6P + 8E cores", "windows", "x86_64", 20, "12×P + 8×E", "x86-64-v3",
          false,
          {{"mc_k2", m(41.7, .03)}, {"mci_k2", m(44.9, .03)}, {"mc_k1", m(83, .03)}, {"mc_k1x", m(388, .04)},
           {"st_k2", m(3.9)}, {"st_k1", m(8.7)}, {"b_k3", m(11.8)}, {"b_k4", m(31.6)}, {"b_k5", m(14.9)},
           {"b_k6", m(11.0)}, {"r_1ms", m(49, .06)}, {"r_10ms", m(90, .02)}, {"c_k4", m(38.8, .04)},
           {"c_k6", m(14.9, .04)}, {"m_lat1", m(104)}, {"m_latn", m(121)}, {"j_idle", tail(520)}, {"j_load", tail(2400)},
           {"u_k2", m(1.04)}, {"u_k3", m(.99)}, {"u_k4", m(1.09)}, {"u_k8f", m(1.55)}, {"u_k8i", m(1.41)}}),
      ref("arm-12c", "Embedded ARM64, 12 cores", "linux", "aarch64", 12, "12×P", "armv8.2-a+dotprod+fp16", false,
          {{"mc_k2", m(15.8, .02)}, {"mci_k2", m(16.4, .02)}, {"mc_k1", m(29, .02)}, {"mc_k1x", m(915, .02)},
           {"st_k2", m(1.45)}, {"st_k1", m(2.9)}, {"b_k3", m(31.2)}, {"b_k4", m(88.0)}, {"b_k5", m(36.5)},
           {"b_k6", m(29.4)}, {"r_1ms", m(71, .03)}, {"r_10ms", m(97)}, {"c_k4", m(91.5, .02)}, {"c_k6", m(31.8, .02)},
           {"m_lat1", m(128)}, {"m_latn", m(141)}, {"j_idle", tail(62)}, {"j_load", tail(240)}, {"u_k2", m(1.01)},
           {"u_k3", m(1.0)}, {"u_k4", m(1.02)}, {"u_k8f", m(1.03)}, {"u_k8i", m(3.4)}}),
      ref("phone-1-3-4", "Phone ARM64, 1 + 3 + 4 cores", "android", "aarch64", 8, "1×P + 3×M + 4×E", "armv9-a+sve2",
          true,
          {{"mc_k2", m(14.2, .05)}, {"mci_k2", m(15.9, .05)}, {"st_k2", m(3.1, .03)}, {"b_k3", m(14.4, .02)},
           {"b_k4", m(36.1, .02)}, {"b_k5", m(17.9, .02)}, {"b_k6", m(12.8, .02)}, {"r_1ms", m(38, .08)},
           {"r_10ms", m(82, .03)}, {"c_k4", m(52.4, .06)}, {"c_k6", m(21.5, .06)}, {"m_lat1", m(165)},
           {"m_latn", m(190)}, {"j_idle", tail(1450)}, {"j_load", tail(4100)}, {"u_k2", m(1.02)}, {"u_k3", m(1.0)},
           {"u_k4", m(1.06)}, {"u_k8f", m(1.12)}, {"u_k8i", m(2.8)}}),
      ref("mini-4c", "Mini PC x86-64, 4 cores", "linux", "x86_64", 4, "4×E", "x86-64-v3", false,
          {{"mc_k2", m(6.9, .02)}, {"mci_k2", m(7.1, .02)}, {"mc_k1", m(14, .02)}, {"mc_k1x", m(1720, .02)},
           {"st_k2", m(1.9)}, {"st_k1", m(3.8)}, {"b_k3", m(24.6)}, {"b_k4", m(66.3)}, {"b_k5", m(30.4)},
           {"b_k6", m(23.0)}, {"r_1ms", m(66, .04)}, {"r_10ms", m(96)}, {"c_k4", m(70.1, .02)}, {"c_k6", m(24.6, .02)},
           {"m_lat1", m(112)}, {"m_latn", m(126)}, {"j_idle", tail(38)}, {"j_load", tail(98)}, {"u_k2", m(1.03)},
           {"u_k3", m(.99)}, {"u_k4", m(1.08)}, {"u_k8f", m(1.48)}, {"u_k8i", m(1.33)}}),
  };
}
