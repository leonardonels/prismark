/*
 * What both compare pages share: the tests, how each value is read from a
 * result, and how two values compare. The same rules as the desktop app
 * (apps/gui/metrics.cpp) and `prismark compare`; tools/site_data.py stops the
 * build if this list and the app's differ.
 *
 * Copyright 2026 The Prismark Authors. Apache-2.0.
 */
'use strict';

const GROUPS = [
    { id: 'mc', name: 'All cores' },
    { id: 'st', name: 'One core' },
    { id: 'mem', name: 'Memory & timing' },
    { id: 'resp', name: 'Responsiveness' },
    { id: 'stress', name: 'Stress test' },
];

/* ---------- reading a value from a result ---------- */

const series = (doc) => doc.analysis?.series ?? [];
const fast = (row) => row.core_type === 'P';
const largestN = (rows) => rows.reduce((best, row) => (!best || row.n > best.n ? row : best), null);

function range(stat, scale = 1) {
    if (!stat || !Number.isFinite(stat.est)) return null;
    return { v: stat.est * scale, lo: stat.lo * scale, hi: stat.hi * scale };
}

/* An all-cores value measured on fewer threads than the machine has is marked, so it does not read as all cores. */
function markThreads(value, doc, row) {
    const all = doc.machine?.cpu?.cores?.length ?? 0;
    if (value && all > 0 && row.n > 0 && row.n < all) {
        value.threads = row.n;
        value.ofThreads = all;
    }
    return value;
}

function throughput(kernel, mode) {
    return (doc) => {
        const rows = series(doc).filter((r) => r.kernel === kernel && r.mode === mode && r.tier === 'baseline' && !r.purpose);
        const row = mode === 'st_sustained' ? rows.find(fast) : largestN(rows);
        if (!row) return null;
        const value = range(row.perf_steady);
        if (value && row.steady_reached === false) value.unsettled = true;
        if (value && row.clamped_windows > 0) value.throttled = true;
        return mode === 'mc_threaded' ? markThreads(value, doc, row) : value;
    };
}

function medianOf(kernel, mode, start, scale) {
    return (doc) => {
        const row = series(doc).find((r) => r.kernel === kernel && r.mode === mode && fast(r) && (!start || r.start === start));
        return row ? range(row.median, scale) : null;
    };
}

function fullBuild(doc) {
    const row = largestN(series(doc).filter((r) => r.kernel === 'K1x'));
    return row ? markThreads(range(row.median, 1e-9), doc, row) : null;
}

function memoryDelay(allCopies) {
    return (doc) => {
        const rows = series(doc).filter((r) => r.kernel === 'K7');
        if (!rows.length) return null;
        // the same 256 MiB on every machine; results from before it was measured keep their largest set
        const sizes = rows.map((r) => r.working_set_bytes);
        const largest = sizes.includes(268435456) ? 268435456 : Math.max(...sizes);
        const at = rows.filter((r) => r.working_set_bytes === largest);
        const row = allCopies ? largestN(at) : at.find((r) => r.n === 1);
        return row ? range(row.median) : null;
    };
}

function timer(condition) {
    return (doc) => {
        const row = (doc.analysis?.periodic ?? []).find((r) => r.condition === condition);
        return row && Number.isFinite(row.p999_ns) ? { v: row.p999_ns / 1e3 } : null;
    };
}

/* Files from before ABI 4 may also hold series measured with changed power settings; only "as configured" counts. */
const asConfigured = (row) => row.cell === undefined || row.cell === 'as_shipped';

function wakeUp(ms) {
    return (doc) => {
        const row = (doc.analysis?.cold_burst ?? []).find((r) => r.W_ms === ms && fast(r) && asConfigured(r));
        return row ? range(row.R_resp, 100) : null;
    };
}

function stress(part) {
    return (doc) => {
        const row = (doc.analysis?.stress ?? []).find((r) => r.part === part);
        if (!row) return null;
        const value = range(row.score);
        const wrong = row.verdict ? row.verdict === 'failed' : (row.checksum_errors || 0) + (row.wrong_tasks || 0) > 0;
        if (value && wrong) value.failed = true;
        return value;
    };
}

/* ---------- the tests, in plain language ---------- */

const METRICS = [
    { id: 'mc_k2', group: 'mc', name: '3D rendering', title: '3D rendering on all cores', sub: 'All cores share one image', unit: 'M samples/s', higher: true, get: throughput('K2', 'mc_threaded') },
    { id: 'mc_k1', group: 'mc', name: 'Compiling code', title: 'Compiling code on all cores', sub: 'All cores compile C++ in memory', unit: 'builds/h', higher: true, get: throughput('K1', 'mc_threaded') },
    { id: 'mc_k1x', group: 'mc', name: 'Full software build', title: 'Full software build with standard tools', sub: 'Real build: CMake + Ninja', unit: 's', higher: false, get: fullBuild },
    { id: 'mc_k12', group: 'mc', name: 'Vector maths', title: 'Vector maths on all cores', sub: 'All cores, widest vectors (AVX / NEON)', unit: 'GFLOP/s', higher: true, get: throughput('K12', 'mc_threaded') },
    { id: 'mc_k13', group: 'mc', name: 'Simulation solver', title: 'Simulation solver on all cores', sub: 'Sparse matrix, limited by memory speed', unit: 'GFLOP/s', higher: true, get: throughput('K13', 'mc_threaded') },
    { id: 'st_k2', group: 'st', name: '3D rendering, one core', title: '3D rendering on one core', sub: 'One core, after warming up', unit: 'M samples/s', higher: true, get: throughput('K2', 'st_sustained') },
    { id: 'st_k1', group: 'st', name: 'Compiling, one core', title: 'Compiling code on one core', sub: 'One core, after warming up', unit: 'builds/h', higher: true, get: throughput('K1', 'st_sustained') },
    { id: 'st_k12', group: 'st', name: 'Vector maths, one core', title: 'Vector maths on one core', sub: 'One core, widest vectors', unit: 'GFLOP/s', higher: true, get: throughput('K12', 'st_sustained') },
    { id: 'b_k3', group: 'st', name: 'Compression', title: 'Compressing a file', sub: 'Compress a small file', unit: 'ms', higher: false, get: medianOf('K3', 'st_burst', null, 1e-6) },
    { id: 'b_k4', group: 'st', name: 'Opening a photo', title: 'Opening a photo', sub: 'Decode and resize a JPEG', unit: 'ms', higher: false, get: medianOf('K4', 'st_burst', null, 1e-6) },
    { id: 'b_k5', group: 'st', name: 'Reading JSON', title: 'Reading structured data (JSON)', sub: 'Parse web-style text data', unit: 'ms', higher: false, get: medianOf('K5', 'st_burst', null, 1e-6) },
    { id: 'b_k6', group: 'st', name: 'Starting a script', title: 'Starting a small script', sub: 'Launch Lua and run a script', unit: 'ms', higher: false, get: medianOf('K6', 'st_burst', null, 1e-6) },
    { id: 'm_lat1', group: 'mem', name: 'Memory delay', title: 'Memory delay', sub: 'One read from main memory', unit: 'ns', higher: false, get: memoryDelay(false) },
    { id: 'm_latn', group: 'mem', name: 'Memory delay, busy', title: 'Memory delay while every core uses memory', sub: 'All cores reading at once', unit: 'ns', higher: false, get: memoryDelay(true) },
    { id: 'j_idle', group: 'mem', name: 'Timer, idle', title: 'Timer punctuality on an idle computer', sub: 'How late a 1 ms timer fires, worst 1 in 1000', unit: 'µs', higher: false, get: timer('idle') },
    { id: 'j_load', group: 'mem', name: 'Timer, busy', title: 'Timer punctuality while other cores are busy', sub: 'The same, other cores busy', unit: 'µs', higher: false, get: timer('loaded') },
    { id: 'r_1ms', group: 'resp', name: 'Wake-up, 1 ms task', title: 'Reacting from rest: a 1 ms task', sub: 'A short task started from rest, against a busy core', unit: '%', higher: true, get: wakeUp(1) },
    { id: 'r_10ms', group: 'resp', name: 'Wake-up, 10 ms task', title: 'Reacting from rest: a 10 ms task', sub: 'A longer task started from rest, against a busy core', unit: '%', higher: true, get: wakeUp(10) },
    { id: 'c_k4', group: 'resp', name: 'Photo from rest', title: 'Opening a photo from rest', sub: 'Decode a JPEG after a pause', unit: 'ms', higher: false, get: medianOf('K4', 'cold_burst', 'cold', 1e-6) },
    { id: 'c_k6', group: 'resp', name: 'Script from rest', title: 'Starting a small script from rest', sub: 'Launch Lua after a pause', unit: 'ms', higher: false, get: medianOf('K6', 'cold_burst', 'cold', 1e-6) },
    { id: 's_st', group: 'stress', name: 'One core', title: 'One core under stress', sub: 'Speed in the 10th minute', unit: 'GFLOP/s', higher: true, get: stress('one_core') },
    { id: 's_mc', group: 'stress', name: 'All cores', title: 'All cores under stress', sub: 'Speed in the 10th minute', unit: 'GFLOP/s', higher: true, get: stress('all_cores') },
    { id: 's_cc', group: 'stress', name: 'Each core in turn', title: 'Each core in turn (core cycling)', sub: 'The slowest core', unit: 'GFLOP/s', higher: true, get: stress('each_core') },
    { id: 's_mem', group: 'stress', name: 'Memory', title: 'Memory under stress', sub: 'Speed in the 10th minute', unit: 'GB/s', higher: true, get: stress('memory') },
];

/* ---------- from a result to what the pages show ---------- */

const OS_NAMES = { linux: 'Linux', windows: 'Windows', macos: 'macOS', android: 'Android', ios: 'iOS', ipados: 'iPadOS' };
const ISA_NAMES = { x86_64: 'x86-64', aarch64: 'ARM64' };

function cleanName(model) {
    return model
        .replace(/\((R|TM)\)/g, '')
        .replace(/\s+@.*$/, '')
        .replace(/\s+w\/.*$/, '')
        .replace(/\s+CPU\b/, '')
        .replace(/\s+\d+-Cores?( Processor)?$/, '')
        .replace(/\s+Processor$/, '')
        .replace(/\s+/g, ' ')
        .trim();
}

function summarize(doc, id, source) {
    const machine = doc.machine ?? {};
    const start = doc.state?.start ?? {};
    const metrics = {};
    for (const metric of METRICS) {
        try {
            const value = metric.get(doc);
            if (value && Number.isFinite(value.v)) metrics[metric.id] = value;
        } catch {
            // A malformed section leaves this test empty.
        }
    }
    const quick = doc.config?.quick ?? (doc.config?.sustained_max_s ?? 600) < 30;
    return {
        id,
        source,
        name: doc.name || cleanName(machine.cpu?.model || 'Unknown CPU'),
        date: doc.started_utc ? doc.started_utc.slice(0, 10) : null,
        version: doc.harness?.version ?? null,
        os: machine.os ?? null,
        isa: machine.isa ?? null,
        threads: machine.cpu?.cores?.length ?? 0,
        cache: machine.cpu?.llc_bytes ?? null,
        ac: start.ac_online ?? null,
        powerMode: start.power_mode ?? null,
        quick: Boolean(quick),
        wrong: (doc.analysis?.checksums ?? []).some((c) => c.consistent === false),
        metrics,
    };
}

const osName = (run) => OS_NAMES[run.os] ?? run.os ?? 'Unknown system';
const isaName = (run) => ISA_NAMES[run.isa] ?? run.isa ?? 'unknown';

function powerName(run) {
    if (run.ac === true) return 'Plugged in';
    if (run.ac === false) return 'On battery';
    return 'Not reported';
}

function describe(run) {
    const parts = [osName(run), isaName(run), `${run.threads} threads`];
    if (run.ac !== null) parts.push(run.ac ? 'plugged in' : 'on battery');
    return parts.join(' · ');
}

/* The same digits as the desktop app. */
function formatValue(v) {
    if (v === null || v === undefined || !Number.isFinite(v)) return '–';
    const size = Math.abs(v);
    if (size >= 1000) return v.toFixed(0);
    if (size >= 100) return v.toFixed(1);
    return v.toPrecision(3).replace(/\.0+$/, '').replace(/(\.\d*?)0+$/, '$1');
}

/*
 * A against B on one test, by the rules of `prismark compare`: the same when
 * the 95 % ranges overlap; percentages by their difference in points; "may be
 * noise" when one side has no range.
 */
function compareValues(metric, a, b) {
    const hasRange = (x) => Number.isFinite(x.lo) && Number.isFinite(x.hi);
    const ranges = hasRange(a) && hasRange(b);
    if (ranges && a.lo <= b.hi && b.lo <= a.hi) return { winner: null, text: 'Same' };
    const speedA = metric.higher ? a.v : 1 / a.v;
    const speedB = metric.higher ? b.v : 1 / b.v;
    if (speedA === speedB) return { winner: null, text: 'Same' };
    const winner = speedA > speedB ? 'a' : 'b';
    const size = metric.unit === '%'
        ? `${Math.abs(a.v - b.v).toFixed(1)} pts`
        : `${(Math.max(speedA, speedB) / Math.min(speedA, speedB)).toFixed(2)}×`;
    return { winner, size, noise: !ranges, text: size };
}

/* ---------- loading: the references, and your own result ---------- */

const YOURS_KEY = 'prismark.site.yours.v1';

function loadYours() {
    try {
        const stored = JSON.parse(localStorage.getItem(YOURS_KEY));
        return stored && stored.source === 'yours' ? stored : null;
    } catch {
        return null;
    }
}

function saveYours(run) {
    try {
        if (run) localStorage.setItem(YOURS_KEY, JSON.stringify(run));
        else localStorage.removeItem(YOURS_KEY);
    } catch {
        // Storage may be blocked: the result then lasts until the page closes.
    }
}

/* Read in this browser only: nothing is sent anywhere. */
async function readResultFile(file) {
    let doc;
    try {
        doc = JSON.parse(await file.text());
    } catch {
        throw new Error('That file is not a Prismark result: it is not JSON.');
    }
    if (doc.schema !== 'prismark/1') throw new Error('That file is not a Prismark result.');
    return summarize(doc, 'yours', 'yours');
}

async function loadReferences() {
    const response = await fetch('references.json');
    if (!response.ok) throw new Error(`references.json: ${response.status}`);
    const docs = await response.json();
    return docs.map((doc) => summarize(doc, doc.file, 'reference'));
}

/* ---------- building the page ---------- */

/* Text always goes in as text, never as HTML: a result file's strings are not ours. */
function el(tag, attrs = {}, ...children) {
    const node = document.createElement(tag);
    for (const [key, value] of Object.entries(attrs)) {
        if (value === false || value === null || value === undefined) continue;
        if (key.startsWith('on')) node.addEventListener(key.slice(2), value);
        else if (key in node && key !== 'class' && key !== 'style') node[key] = value;
        else node.setAttribute(key, value);
    }
    node.append(...children.filter((child) => child !== null && child !== undefined && child !== false));
    return node;
}

const metricById = (id) => METRICS.find((metric) => metric.id === id);
const rankable = (run) => !run.quick && !run.wrong;

function valueTags(value) {
    const tags = [];
    if (value.throttled) tags.push('throttled');
    if (value.unsettled) tags.push('not settled');
    if (value.threads) tags.push(`${value.threads} of ${value.ofThreads} threads`);
    return tags;
}
