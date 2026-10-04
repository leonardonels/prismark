/*
 * Reference systems for the ranking.
 *
 * PLACEHOLDER VALUES. These entries are not measurements: they only give the
 * viewer something to rank against until real reference results exist. Each
 * is flagged `placeholder: true`, and the viewer labels them as such.
 *
 * To add a real reference: open a result file in the viewer, select it,
 * press "Export summary", and add the exported object to this list (it is
 * already in the prismark-summary/1 form, with placeholder: false).
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
(function () {
  'use strict';
  /* m(value, relative half-width of the CI): a placeholder metric with a plausible interval. */
  const m = (v, rel = 0.01) => ({ v, lo: v * (1 - rel), hi: v * (1 + rel) });
  const tail = (v) => ({ v });
  const ref = (id, name, machine, metrics) => ({
    schema: 'prismark-summary/1', id: `placeholder-${id}`, name, placeholder: true,
    date: '2026-10-04', complete: true, verified: false, quick: false, machine, metrics,
    tiers: { baseline: { level: machine.isa === 'aarch64' ? 'armv8.2-a' : 'x86-64-v2' }, max: { level: machine.max } },
    unavailable: machine.mobile ? [{ kernel: 'K1', mode: 'all', reason: 'desktop platforms only' }, { kernel: 'K1x', mode: 'mc_threaded', reason: 'desktop platforms only' }] : [],
    curves: { cold: [], scaling: [] }, ratios: [], checksums_ok: true,
  });

  window.PRISMARK_REFERENCES = [
    ref('desktop-16c', 'Desktop x86-64, 16 cores / 32 threads',
      { os: 'linux', isa: 'x86_64', ncpu: 32, types: { P: 32 }, max: 'x86-64-v4' },
      { mc_k2: m(74, 0.02), mci_k2: m(79, 0.02), mc_k1: m(152, 0.02), mc_k1x: m(171, 0.02), st_k2: m(4.3), st_k1: m(9.8),
        b_k3: m(10.6), b_k4: m(27.9), b_k5: m(13.1), b_k6: m(9.7), r_1ms: m(0.64, 0.04), r_10ms: m(0.95, 0.01),
        c_k4: m(30.8, 0.02), c_k6: m(11.1, 0.03), m_lat1: m(76), m_latn: m(94), j_idle: tail(41), j_load: tail(155),
        u_k2: m(1.07), u_k3: m(0.98), u_k4: m(1.14), u_k8f: m(2.71), u_k8i: m(2.05) }),
    ref('laptop-8c', 'Laptop x86-64, 8 cores / 16 threads',
      { os: 'linux', isa: 'x86_64', ncpu: 16, types: { P: 16 }, max: 'x86-64-v4' },
      { mc_k2: m(38.5, 0.03), mci_k2: m(40.2, 0.03), mc_k1: m(78, 0.03), mc_k1x: m(342, 0.03), st_k2: m(3.6), st_k1: m(8.1),
        b_k3: m(12.9), b_k4: m(34.5), b_k5: m(16.4), b_k6: m(12.3), r_1ms: m(0.57, 0.05), r_10ms: m(0.93, 0.01),
        c_k4: m(36.9, 0.03), c_k6: m(13.6, 0.03), m_lat1: m(97), m_latn: m(108), j_idle: tail(260), j_load: tail(1880),
        u_k2: m(1.05), u_k3: m(0.96), u_k4: m(1.12), u_k8f: m(2.9), u_k8i: m(1.97) }),
    ref('hybrid-6p8e', 'Hybrid laptop x86-64, 6P + 8E cores',
      { os: 'windows', isa: 'x86_64', ncpu: 20, types: { P: 12, E: 8 }, max: 'x86-64-v3' },
      { mc_k2: m(41.7, 0.03), mci_k2: m(44.9, 0.03), mc_k1: m(83, 0.03), mc_k1x: m(388, 0.04), st_k2: m(3.9), st_k1: m(8.7),
        b_k3: m(11.8), b_k4: m(31.6), b_k5: m(14.9), b_k6: m(11.0), r_1ms: m(0.49, 0.06), r_10ms: m(0.9, 0.02),
        c_k4: m(38.8, 0.04), c_k6: m(14.9, 0.04), m_lat1: m(104), m_latn: m(121), j_idle: tail(520), j_load: tail(2400),
        u_k2: m(1.04), u_k3: m(0.99), u_k4: m(1.09), u_k8f: m(1.55), u_k8i: m(1.41) }),
    ref('arm-12c', 'Embedded ARM64, 12 cores',
      { os: 'linux', isa: 'aarch64', ncpu: 12, types: { P: 12 }, max: 'armv8.2-a+dotprod+fp16' },
      { mc_k2: m(15.8, 0.02), mci_k2: m(16.4, 0.02), mc_k1: m(29, 0.02), mc_k1x: m(915, 0.02), st_k2: m(1.45), st_k1: m(2.9),
        b_k3: m(31.2), b_k4: m(88.0), b_k5: m(36.5), b_k6: m(29.4), r_1ms: m(0.71, 0.03), r_10ms: m(0.97, 0.01),
        c_k4: m(91.5, 0.02), c_k6: m(31.8, 0.02), m_lat1: m(128), m_latn: m(141), j_idle: tail(62), j_load: tail(240),
        u_k2: m(1.01), u_k3: m(1.0), u_k4: m(1.02), u_k8f: m(1.03), u_k8i: m(3.4) }),
    ref('phone-1-3-4', 'Phone ARM64, 1 + 3 + 4 cores',
      { os: 'android', isa: 'aarch64', ncpu: 8, types: { P: 1, M: 3, E: 4 }, max: 'armv9-a+sve2', mobile: true },
      { mc_k2: m(14.2, 0.05), mci_k2: m(15.9, 0.05), st_k2: m(3.1, 0.03),
        b_k3: m(14.4, 0.02), b_k4: m(36.1, 0.02), b_k5: m(17.9, 0.02), b_k6: m(12.8, 0.02), r_1ms: m(0.38, 0.08), r_10ms: m(0.82, 0.03),
        c_k4: m(52.4, 0.06), c_k6: m(21.5, 0.06), m_lat1: m(165), m_latn: m(190), j_idle: tail(1450), j_load: tail(4100),
        u_k2: m(1.02), u_k3: m(1.0), u_k4: m(1.06), u_k8f: m(1.12), u_k8i: m(2.8) }),
    ref('mini-4c', 'Mini PC x86-64, 4 cores',
      { os: 'linux', isa: 'x86_64', ncpu: 4, types: { E: 4 }, max: 'x86-64-v3' },
      { mc_k2: m(6.9, 0.02), mci_k2: m(7.1, 0.02), mc_k1: m(14, 0.02), mc_k1x: m(1720, 0.02), st_k2: m(1.9), st_k1: m(3.8),
        b_k3: m(24.6), b_k4: m(66.3), b_k5: m(30.4), b_k6: m(23.0), r_1ms: m(0.66, 0.04), r_10ms: m(0.96, 0.01),
        c_k4: m(70.1, 0.02), c_k6: m(24.6, 0.02), m_lat1: m(112), m_latn: m(126), j_idle: tail(38), j_load: tail(98),
        u_k2: m(1.03), u_k3: m(0.99), u_k4: m(1.08), u_k8f: m(1.48), u_k8i: m(1.33) }),
  ];
})();
