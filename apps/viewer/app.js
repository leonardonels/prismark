/*
 * Prismark viewer. Shows result files as per-test rankings, in the manner of
 * Cinebench, without ever combining tests into one score: each test is one
 * kernel in one mode, and every number shown is read from the statistics the
 * C core computed (the "analysis" section), so all front-ends agree.
 *
 * A static page (spec 7.3): open it from disk, load or drop result files
 * from any device; summaries are kept in this browser. To run the benchmark,
 * use the desktop app (apps/gui) or the CLI.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
'use strict';

const STORE_KEY = 'prismark.results.v1';
const SUMMARY_SCHEMA = 'prismark-summary/1';

/* ---------- formatting ---------- */

const num = (v, digits = 3) => {
  if (v == null || !Number.isFinite(v)) return '–';
  const a = Math.abs(v);
  if (a >= 1000) return v.toFixed(0);
  if (a >= 100) return v.toFixed(1);
  return Number(v.toPrecision(digits)).toString();
};
const esc = (s) => String(s ?? '').replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const bytes = (b) => (b >= 1 << 20 ? `${num(b / (1 << 20))} MiB` : `${num(b / 1024)} KiB`);

/* ---------- metric catalogue ---------- */

const GROUPS = [
  { id: 'mc', label: 'Multi core' },
  { id: 'st', label: 'Single core' },
  { id: 'resp', label: 'Responsiveness' },
  { id: 'mem', label: 'Memory & jitter' },
  { id: 'isa', label: 'ISA uplift' },
];

const series = (d) => d?.analysis?.series ?? [];
const ci = (o, k = 1) => (o && Number.isFinite(o.est) ? { v: o.est * k, lo: o.lo * k, hi: o.hi * k } : null);
const largestN = (rows) => rows.reduce((best, r) => (!best || r.n > best.n ? r : best), null);
const fast = (r) => r.core_type === 'P';

function throughput(kernel, mode, extra = () => true) {
  return (d) => {
    const rows = series(d).filter((r) => r.kernel === kernel && r.mode === mode && r.tier === 'baseline' && !r.purpose && extra(r));
    const r = mode === 'st_sustained' ? rows.find(fast) : largestN(rows);
    return r ? ci(r.perf_steady) : null;
  };
}

function burstTime(kernel) {
  return (d) => {
    const r = series(d).find((x) => x.kernel === kernel && x.mode === 'st_burst' && fast(x));
    return r ? ci(r.median, 1e-6) : null;
  };
}

function coldTime(kernel) {
  return (d) => {
    const r = series(d).find((x) => x.kernel === kernel && x.mode === 'cold_burst' && x.start === 'cold' && fast(x));
    return r ? ci(r.median, 1e-6) : null;
  };
}

function uplift(kernel, variant) {
  return (d) => {
    const r = (d?.analysis?.ratios ?? []).find((x) => x.name === 'U_ISA' && x.kernel === kernel && (x.variant ?? null) === variant);
    return r ? ci(r.value) : null;
  };
}

function k7(nPick) {
  return (d) => {
    const rows = series(d).filter((r) => r.kernel === 'K7');
    if (!rows.length) return null;
    const ws = Math.max(...rows.map((r) => r.working_set_bytes));
    const at = rows.filter((r) => r.working_set_bytes === ws);
    const r = nPick === 'max' ? largestN(at) : at.find((x) => x.n === 1);
    return r ? ci(r.median) : null;
  };
}

function periodic(condition) {
  return (d) => {
    const r = (d?.analysis?.periodic ?? []).find((x) => x.condition === condition);
    return r && Number.isFinite(r.p999_ns) ? { v: r.p999_ns / 1e3 } : null;
  };
}

// Files from before ABI 4 may also hold series measured with changed power settings; only "as configured" counts.
const asConfigured = (x) => x.cell === undefined || x.cell === 'as_shipped';

function rresp(wms) {
  return (d) => {
    const r = (d?.analysis?.cold_burst ?? []).find((x) => x.W_ms === wms && fast(x) && asConfigured(x));
    return r ? ci(r.R_resp) : null;
  };
}

const METRICS = [
  { id: 'mc_k2', group: 'mc', name: 'Render', sub: 'K2 path tracer · MC threaded · all threads', unit: 'Msamples/s', better: 'higher',
    desc: 'Steady-state throughput of the path tracer with every hardware thread sharing each frame.', get: throughput('K2', 'mc_threaded') },
  { id: 'mc_k1', group: 'mc', name: 'Compile', sub: 'K1 in-process Clang · MC threaded', unit: 'builds/h', better: 'higher',
    desc: 'Compiling the K1x translation units in memory on every thread. Desktop only.', get: throughput('K1', 'mc_threaded') },
  { id: 'mc_k1x', group: 'mc', name: 'Full build', sub: 'K1x CMake + Ninja · all threads', unit: 's', better: 'lower',
    desc: 'Clean cross-build of the pinned LLVM subset, process creation and linking included.',
    get: (d) => { const r = largestN(series(d).filter((x) => x.kernel === 'K1x')); return r ? ci(r.median, 1e-9) : null; } },

  { id: 'st_k2', group: 'st', name: 'Render', sub: 'K2 · ST sustained · fastest core', unit: 'Msamples/s', better: 'higher',
    desc: 'Single-core throughput at thermal steady state.', get: throughput('K2', 'st_sustained') },
  { id: 'st_k1', group: 'st', name: 'Compile', sub: 'K1 · ST sustained · fastest core', unit: 'builds/h', better: 'higher',
    desc: 'Single-core in-process compile at steady state.', get: throughput('K1', 'st_sustained') },
  { id: 'b_k3', group: 'st', name: 'Compression', sub: 'K3 zstd · ST burst', unit: 'ms', better: 'lower',
    desc: 'Time per job on a pre-warmed fastest core: short interactive work.', get: burstTime('K3') },
  { id: 'b_k4', group: 'st', name: 'Image decode', sub: 'K4 JPEG + resize · ST burst', unit: 'ms', better: 'lower',
    desc: 'Time per job on a pre-warmed fastest core.', get: burstTime('K4') },
  { id: 'b_k5', group: 'st', name: 'JSON parsing', sub: 'K5 · ST burst', unit: 'ms', better: 'lower',
    desc: 'Time per job on a pre-warmed fastest core.', get: burstTime('K5') },
  { id: 'b_k6', group: 'st', name: 'Script start', sub: 'K6 Lua · ST burst', unit: 'ms', better: 'lower',
    desc: 'New interpreter, load and run a script: time per job.', get: burstTime('K6') },

  { id: 'r_1ms', group: 'resp', name: 'Responsiveness at 1 ms', sub: 'K9 · R_resp = t_warm / t_cold', unit: '', better: 'higher',
    desc: '1 means work issued from idle runs as fast as on a busy core; lower means a cold-start penalty.', get: rresp(1) },
  { id: 'r_10ms', group: 'resp', name: 'Responsiveness at 10 ms', sub: 'K9 · R_resp', unit: '', better: 'higher',
    desc: 'The same ratio for a longer task; frequency ramp penalties shrink as W grows.', get: rresp(10) },
  { id: 'c_k4', group: 'resp', name: 'Image decode from idle', sub: 'K4 · cold burst', unit: 'ms', better: 'lower',
    desc: 'One burst job after a random 50–500 ms idle gap, timed from the deadline.', get: coldTime('K4') },
  { id: 'c_k6', group: 'resp', name: 'Script start from idle', sub: 'K6 · cold burst', unit: 'ms', better: 'lower',
    desc: 'One burst job after a random idle gap, timed from the deadline.', get: coldTime('K6') },

  { id: 'm_lat1', group: 'mem', name: 'Memory latency', sub: 'K7 · largest working set · 1 copy', unit: 'ns', better: 'lower',
    desc: 'Dependent loads beyond the last-level cache.', get: k7(1) },
  { id: 'm_latn', group: 'mem', name: 'Memory latency, contended', sub: 'K7 · largest working set · all copies', unit: 'ns', better: 'lower',
    desc: 'The same chase while every other CPU chases too.', get: k7('max') },
  { id: 'j_idle', group: 'mem', name: 'Wake-up lateness, idle', sub: 'K10 periodic · p99.9', unit: 'µs', better: 'lower',
    desc: 'Tail of the lateness of a 1 ms periodic task. Tails are never averaged.', get: periodic('idle') },
  { id: 'j_load', group: 'mem', name: 'Wake-up lateness, loaded', sub: 'K10 periodic · p99.9 with K7 load', unit: 'µs', better: 'lower',
    desc: 'The same tail with K7 loading every other CPU.', get: periodic('loaded') },

  { id: 'u_k2', group: 'isa', name: 'K2 render', sub: 'U_ISA = perf_max / perf_baseline', unit: '×', better: 'higher',
    desc: 'Same source, baseline ISA against the highest supported level.', get: uplift('K2', null) },
  { id: 'u_k3', group: 'isa', name: 'K3 compression', sub: 'U_ISA', unit: '×', better: 'higher', desc: 'ISA uplift of zstd.', get: uplift('K3', null) },
  { id: 'u_k4', group: 'isa', name: 'K4 image', sub: 'U_ISA', unit: '×', better: 'higher', desc: 'ISA uplift of decode and resize.', get: uplift('K4', null) },
  { id: 'u_k8f', group: 'isa', name: 'K8 matmul FP32', sub: 'U_ISA', unit: '×', better: 'higher', desc: 'ISA uplift of FP32 matrix multiply.', get: uplift('K8', 'fp32') },
  { id: 'u_k8i', group: 'isa', name: 'K8 matmul INT8', sub: 'U_ISA', unit: '×', better: 'higher', desc: 'ISA uplift of INT8 matrix multiply.', get: uplift('K8', 'int8') },
];

/* ---------- summaries ---------- */

/* Reduces a result document to what the viewer needs; references are stored in this form. */
function summarize(doc, source) {
  const m = doc.machine ?? {};
  const cores = m.cpu?.cores ?? [];
  const types = {};
  for (const c of cores) types[c.type] = (types[c.type] ?? 0) + 1;
  const metrics = {};
  for (const mt of METRICS) {
    try {
      const v = mt.get(doc);
      if (v && Number.isFinite(v.v)) metrics[mt.id] = v;
    } catch { /* a malformed section leaves the metric empty */ }
  }
  const an = doc.analysis ?? {};
  return {
    schema: SUMMARY_SCHEMA,
    id: doc.run_id,
    name: m.cpu?.model || 'Unknown CPU',
    source,
    placeholder: false,
    date: doc.started_utc ? doc.started_utc.slice(0, 10) : null,
    complete: !!doc.complete,
    verified: !!doc.verified,
    quick: (doc.config?.sustained_max_s ?? 600) < 30,
    machine: {
      os: m.os, kernel: m.kernel, isa: m.isa, board: m.board, ncpu: cores.length, types,
      llc_bytes: m.cpu?.llc_bytes, capabilities: m.capabilities,
    },
    harness: doc.harness,
    frontend: doc.frontend,
    tiers: doc.tiers,
    state: { power_mode: doc.state?.start?.power_mode ?? null, ac_online: doc.state?.start?.ac_online ?? null,
             governor: doc.state?.start?.cpufreq?.[0]?.governor ?? null },
    unavailable: doc.unavailable ?? [],
    metrics,
    curves: {
      cold: (an.cold_burst ?? []).filter(asConfigured).map((r) => ({ t: r.core_type, w: r.W_ms, r: r.R_resp })),
      scaling: (an.scaling ?? []).map((r) => ({ k: r.kernel, n: r.n, S: r.S, p: r.p })),
    },
    ratios: (an.ratios ?? []).map((r) => ({ name: r.name, kernel: r.kernel, variant: r.variant, n: r.n, core: r.core_type, value: r.value })),
    checksums_ok: (an.checksums ?? []).every((c) => c.consistent),
  };
}

/* ---------- state ---------- */

const state = {
  runs: [],          /* summaries: references, then loaded results */
  group: 'mc',
  metric: 'mc_k2',
  selected: null,    /* run id */
};

function loadStored() {
  try {
    const v = JSON.parse(localStorage.getItem(STORE_KEY) || '[]');
    return Array.isArray(v) ? v.filter((s) => s && s.schema === SUMMARY_SCHEMA) : [];
  } catch {
    return [];
  }
}

function saveStored() {
  try {
    localStorage.setItem(STORE_KEY, JSON.stringify(state.runs.filter((r) => r.source !== 'reference')));
  } catch { /* storage may be unavailable (private window); runs then last for this session */ }
}

function addRun(summary) {
  const i = state.runs.findIndex((r) => r.id === summary.id && r.source !== 'reference');
  if (i >= 0) state.runs[i] = summary;
  else state.runs.push(summary);
  saveStored();
}

function isYours(r) { return r.source !== 'reference'; }

/* ---------- rendering: tests ---------- */

const $ = (id) => document.getElementById(id);

function renderTabs() {
  $('tabs').innerHTML = GROUPS.map((g) =>
    `<button class="tab" role="tab" data-group="${g.id}" aria-selected="${g.id === state.group}">${esc(g.label)}</button>`).join('');
}

function bestOfYours(mt) {
  const vals = state.runs.filter(isYours).map((r) => r.metrics[mt.id]?.v).filter(Number.isFinite);
  if (!vals.length) return null;
  return mt.better === 'higher' ? Math.max(...vals) : Math.min(...vals);
}

function renderTests() {
  const items = METRICS.filter((m) => m.group === state.group);
  $('testList').innerHTML = items.map((m) => {
    const best = bestOfYours(m);
    return `<li><button class="test" data-metric="${m.id}" aria-current="${m.id === state.metric}">
      <span class="name">${esc(m.name)}</span><span class="sub">${esc(m.sub)}</span>
      <span class="best">${best == null ? 'no result yet' : `yours: ${num(best)} ${esc(m.unit)}`}</span></button></li>`;
  }).join('');
}

/* ---------- rendering: ranking ---------- */

function renderRanking() {
  const mt = METRICS.find((m) => m.id === state.metric);
  $('testTitle').textContent = `${mt.name} — ${mt.sub}`;
  $('testDesc').textContent = `${mt.desc} ${mt.better === 'higher' ? 'Higher is better.' : 'Lower is better.'}`;
  const rows = state.runs.filter((r) => r.metrics[mt.id]).map((r) => ({ run: r, m: r.metrics[mt.id] }));
  if (!rows.length) {
    $('bars').innerHTML = `<li class="empty">No result has this test yet. Load a result file.</li>`;
    $('rankNote').textContent = '';
    return;
  }
  /* Bars are proportional to speed: the value itself, or its inverse when lower is better. */
  const speed = (v) => (mt.better === 'higher' ? v : 1 / v);
  rows.sort((a, b) => speed(b.m.v) - speed(a.m.v));
  const top = Math.max(...rows.map((x) => speed(Number.isFinite(x.m.hi) && mt.better === 'higher' ? x.m.hi : x.m.v)),
                       ...rows.map((x) => speed(Number.isFinite(x.m.lo) && mt.better === 'lower' ? x.m.lo : x.m.v)));
  const ref = rows.find((x) => x.run.id === state.selected) ?? rows.find((x) => isYours(x.run));
  $('bars').innerHTML = rows.map((x, i) => {
    const r = x.run;
    const w = (100 * speed(x.m.v)) / top;
    let ciHtml = '';
    if (Number.isFinite(x.m.lo) && Number.isFinite(x.m.hi)) {
      const a = (100 * speed(x.m.lo)) / top, b = (100 * speed(x.m.hi)) / top;
      ciHtml = `<span class="ci" style="left:${Math.min(a, b)}%;width:${Math.abs(b - a)}%"></span>`;
    }
    const rel = ref && ref !== x ? `${speed(x.m.v) / speed(ref.m.v) >= 1 ? '+' : ''}${num(100 * (speed(x.m.v) / speed(ref.m.v) - 1), 2)}%` : '';
    const tags = [r.placeholder ? 'placeholder' : '', r.quick ? 'quick' : '', !r.complete ? 'partial' : ''].filter(Boolean)
      .map((t) => `<span class="tag">${t}</span>`).join('');
    return `<li class="bar ${isYours(r) ? 'you' : ''} ${r.id === state.selected ? 'selected' : ''}" data-run="${esc(r.id)}" data-source="${esc(r.source)}" tabindex="0">
      <span class="rank">${i + 1}</span>
      <div>
        <div class="label"><span class="who">${esc(r.name)}${tags}</span>
          <span class="val">${num(x.m.v)} ${esc(mt.unit)}${rel ? `<span class="rel">${rel}</span>` : ''}</span></div>
        <div class="track"><span class="fill" style="width:${w}%"></span>${ciHtml}</div>
      </div></li>`;
  }).join('');
  $('rankNote').textContent = 'Whiskers: 95 % bootstrap CI of the median. Percentages are relative to the selected result. ' +
    'Results of different modes are never combined; compare two runs with `prismark compare` for per-kernel ratios and profiles.';
}

/* ---------- rendering: details ---------- */

function svgChart({ lines, xlog, xlabel, ylabel, ymin, ymax, ideal }) {
  const W = 340, H = 200, L = 40, R = 8, T = 10, B = 32;
  const pts = lines.flatMap((l) => l.pts);
  if (!pts.length) return '';
  const xs = pts.map((p) => p[0]);
  let x0 = Math.min(...xs), x1 = Math.max(...xs);
  if (x0 === x1) x1 = x0 + 1;
  const ys = pts.flatMap((p) => [p[1], p[2], p[3]]).filter(Number.isFinite);
  let y0 = ymin ?? Math.min(0, ...ys), y1 = ymax ?? Math.max(...ys) * 1.05;
  if (ideal) y1 = Math.max(y1, x1);
  if (y0 === y1) y1 = y0 + 1;
  const fx = (x) => L + ((xlog ? Math.log(x / x0) / Math.log(x1 / x0) : (x - x0) / (x1 - x0)) * (W - L - R));
  const fy = (y) => T + (1 - (y - y0) / (y1 - y0)) * (H - T - B);
  let s = `<svg class="chart" viewBox="0 0 ${W} ${H}" role="img" aria-label="${esc(ylabel)} against ${esc(xlabel)}">`;
  s += `<line class="axis" x1="${L}" y1="${H - B}" x2="${W - R}" y2="${H - B}"/><line class="axis" x1="${L}" y1="${T}" x2="${L}" y2="${H - B}"/>`;
  const xticks = xlog ? [...new Set(xs)].sort((a, b) => a - b) : [...new Set(xs)];
  xticks.forEach((t, i) => {
    const anchor = i === xticks.length - 1 ? 'end' : i === 0 ? 'start' : 'middle';
    s += `<text x="${fx(t)}" y="${H - B + 12}" text-anchor="${anchor}">${num(t, 2)}</text>`;
  });
  for (const t of [y0, (y0 + y1) / 2, y1]) s += `<text x="${L - 4}" y="${fy(t) + 3}" text-anchor="end">${num(t, 2)}</text>`;
  s += `<text x="${(L + W - R) / 2}" y="${H - 4}" text-anchor="middle">${esc(xlabel)}</text>`;
  if (ideal) s += `<line class="ideal" x1="${fx(x0)}" y1="${fy(x0)}" x2="${fx(x1)}" y2="${fy(x1)}"/>`;
  lines.forEach((l, i) => {
    const c = i % 5;
    const d = l.pts.map((p, k) => `${k ? 'L' : 'M'}${fx(p[0]).toFixed(1)},${fy(p[1]).toFixed(1)}`).join('');
    s += `<path class="s${c}" d="${d}" fill="none" stroke-width="1.6"/>`;
    for (const p of l.pts) {
      if (Number.isFinite(p[2]) && Number.isFinite(p[3]))
        s += `<line class="s${c}" x1="${fx(p[0])}" x2="${fx(p[0])}" y1="${fy(p[2])}" y2="${fy(p[3])}" stroke-width="1"/>`;
      s += `<circle class="dot${c}" cx="${fx(p[0])}" cy="${fy(p[1])}" r="2.4"/>`;
    }
  });
  s += '</svg>';
  const colors = ['var(--accent)', '#e0605a', '#46b07a', '#d89a2b', 'var(--muted)'];
  const legend = lines.map((l, i) => `<span class="chip"><i class="sw" style="background:${colors[i % 5]}"></i>${esc(l.name)}</span>`).join('');
  return s + `<div class="chips">${legend}</div><p class="muted small">${esc(ylabel)}</p>`;
}

function renderDetails() {
  const r = state.runs.find((x) => x.id === state.selected);
  const el = $('details');
  if (!r) {
    el.innerHTML = '<p class="muted">Select a result to see its machine state, capabilities and curves.</p>';
    return;
  }
  const m = r.machine ?? {};
  const caps = m.capabilities ?? {};
  const typeStr = Object.entries(m.types ?? {}).map(([t, n]) => `${n}×${t}`).join(' + ');
  const capChips = ['pinning', 'perf_counters', 'qos_only']
    .filter((k) => k in caps).map((k) => `<span class="chip ${caps[k] ? 'on' : 'off'}">${k.replace('_', ' ')}</span>`).join('');
  const rowsHtml = METRICS.filter((mt) => r.metrics[mt.id]).map((mt) => {
    const v = r.metrics[mt.id];
    const range = Number.isFinite(v.lo) ? ` <span class="muted">[${num(v.lo)}, ${num(v.hi)}]</span>` : '';
    return `<tr><td>${esc(mt.name)}<div class="muted small">${esc(mt.sub)}</div></td><td class="num">${num(v.v)} ${esc(mt.unit)}${range}</td></tr>`;
  }).join('');

  let charts = '';
  const cold = (r.curves?.cold ?? []).filter((c) => c.t === 'P');
  if (cold.length) {
    charts += '<h4>Responsiveness curve</h4>' + svgChart({
      lines: [{ name: 'as configured', pts: cold.slice().sort((a, b) => a.w - b.w).map((c) => [c.w, c.r.est, c.r.lo, c.r.hi]) }],
      xlog: true, xlabel: 'W (ms)', ylabel: 'R_resp = t_warm / t_cold', ymin: 0, ymax: 1.05,
    });
  }
  const sc = r.curves?.scaling ?? [];
  if (sc.length) {
    const ks = [...new Set(sc.map((s) => s.k))];
    charts += '<h4>Scaling</h4>' + svgChart({
      lines: ks.map((k) => ({ name: k, pts: sc.filter((s) => s.k === k).sort((a, b) => a.n - b.n).map((s) => [s.n, s.S.est, s.S.lo, s.S.hi]) })),
      xlabel: 'threads n', ylabel: 'S(n) = T1 / Tn (dashed: ideal)', ymin: 0, ideal: true,
    });
  }
  const ratios = (r.ratios ?? []).filter((x) => x.name !== 'U_ISA').map((x) =>
    `<tr><td>${esc(x.name)} ${esc(x.kernel)}${x.variant ? ' ' + esc(x.variant) : ''}${x.n ? ` n=${x.n}` : ''}</td>
     <td class="num">${num(x.value.est)} <span class="muted">[${num(x.value.lo)}, ${num(x.value.hi)}]</span></td></tr>`).join('');
  const unavailable = (r.unavailable ?? []).map((u) => `<li>${esc(u.kernel)}${u.variant ? ' ' + esc(u.variant) : ''} · ${esc(u.mode)}: <span class="muted">${esc(u.reason)}</span></li>`).join('');

  el.innerHTML = `
    <h3>${esc(r.name)}</h3>
    <p class="muted small">${r.placeholder ? '<strong class="warn">Placeholder values, not a measurement.</strong> ' : ''}
      ${esc(r.source)} · ${esc(r.date ?? '')}${r.quick ? ' · quick run' : ''}${r.complete ? '' : ' · <span class="warn">incomplete</span>'}
      · ${r.verified ? 'verified build' : 'unverified build'}</p>
    <dl class="kv">
      <dt>CPU</dt><dd>${m.ncpu ?? '?'} CPUs${typeStr ? ` (${esc(typeStr)})` : ''}, ${esc(m.isa ?? '')}</dd>
      <dt>OS</dt><dd>${esc(m.os ?? '')} ${esc(m.kernel ?? '')}</dd>
      ${m.board ? `<dt>Board</dt><dd>${esc(m.board)}</dd>` : ''}
      ${m.llc_bytes ? `<dt>LLC</dt><dd>${bytes(m.llc_bytes)}</dd>` : ''}
      <dt>Tiers</dt><dd>${esc(r.tiers?.baseline?.level ?? 'baseline')}; max ${esc(r.tiers?.max?.level ?? r.tiers?.max?.status ?? '–')}</dd>
      ${r.state?.governor ? `<dt>Governor</dt><dd>${esc(r.state.governor)}</dd>` : ''}
      ${r.state?.power_mode ? `<dt>Power mode</dt><dd>${esc(r.state.power_mode)}</dd>` : ''}
      ${r.harness ? `<dt>Harness</dt><dd class="mono small">${esc(r.harness.version)} · ${esc(r.harness.compiler)} · ${esc(r.harness.git)}</dd>` : ''}
      ${r.frontend ? `<dt>Front-end</dt><dd>${esc(r.frontend.kind)} (${esc(r.frontend.ui_state)})</dd>` : ''}
      <dt>Checksums</dt><dd>${r.checksums_ok ? 'consistent' : '<span class="warn">inconsistent</span>'}</dd>
    </dl>
    <h4>Capabilities</h4><div class="chips">${capChips || '<span class="muted small">not recorded</span>'}</div>
    <h4>Tests</h4><table class="tbl"><tbody>${rowsHtml || '<tr><td class="muted">none</td></tr>'}</tbody></table>
    ${charts}
    ${ratios ? `<h4>Same-kernel ratios</h4><table class="tbl"><tbody>${ratios}</tbody></table>` : ''}
    ${unavailable ? `<h4>Unavailable</h4><ul class="small">${unavailable}</ul>` : ''}
    <div class="detail-actions">
      <button class="btn" id="exportBtn">Export summary</button>
      ${isYours(r) ? '<button class="btn" id="removeBtn">Remove</button>' : ''}
    </div>`;
  $('exportBtn').onclick = () => download(`prismark-summary-${r.id}.json`, JSON.stringify({ ...r, source: 'reference' }, null, 1));
  if ($('removeBtn')) $('removeBtn').onclick = () => {
    state.runs = state.runs.filter((x) => !(x.id === r.id && isYours(x)));
    state.selected = null;
    saveStored();
    render();
  };
}

function download(name, text) {
  const a = document.createElement('a');
  a.href = URL.createObjectURL(new Blob([text], { type: 'application/json' }));
  a.download = name;
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 1000);
}

function render() {
  renderTabs();
  renderTests();
  renderRanking();
  renderDetails();
  $('placeholderBanner').hidden = !state.runs.some((r) => r.placeholder);
}

/* ---------- input ---------- */

async function addFiles(files) {
  let last = null;
  for (const f of files) {
    try {
      const doc = JSON.parse(await f.text());
      if (doc.schema === SUMMARY_SCHEMA) { /* an exported summary */
        addRun({ ...doc, source: 'local' });
        last = doc.id;
      } else if (doc.schema === 'prismark/1') {
        const s = summarize(doc, 'local');
        addRun(s);
        last = s.id;
      } else {
        alert(`${f.name}: not a Prismark result file`);
      }
    } catch (e) {
      alert(`${f.name}: ${e.message}`);
    }
  }
  if (last) state.selected = last;
  render();
}

/* ---------- wiring ---------- */

function init() {
  const refs = (window.PRISMARK_REFERENCES ?? []).map((r) => ({ ...r, source: 'reference' }));
  state.runs = [...refs, ...loadStored()];
  const mine = state.runs.filter(isYours);
  if (mine.length) state.selected = mine[mine.length - 1].id;

  $('tabs').addEventListener('click', (e) => {
    const b = e.target.closest('.tab');
    if (!b) return;
    state.group = b.dataset.group;
    state.metric = METRICS.find((m) => m.group === state.group).id;
    render();
  });
  $('testList').addEventListener('click', (e) => {
    const b = e.target.closest('.test');
    if (!b) return;
    state.metric = b.dataset.metric;
    render();
  });
  const pickBar = (e) => {
    const b = e.target.closest('.bar');
    if (!b) return;
    state.selected = b.dataset.run;
    render();
  };
  $('bars').addEventListener('click', pickBar);
  $('bars').addEventListener('keydown', (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); pickBar(e); } });
  $('file').addEventListener('change', (e) => addFiles([...e.target.files]));

  let depth = 0;
  window.addEventListener('dragenter', (e) => { e.preventDefault(); depth++; $('drop').hidden = false; });
  window.addEventListener('dragleave', () => { if (--depth <= 0) { depth = 0; $('drop').hidden = true; } });
  window.addEventListener('dragover', (e) => e.preventDefault());
  window.addEventListener('drop', (e) => {
    e.preventDefault();
    depth = 0;
    $('drop').hidden = true;
    addFiles([...e.dataTransfer.files]);
  });

  render();
}

document.addEventListener('DOMContentLoaded', init);
