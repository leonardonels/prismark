#!/usr/bin/env python3
"""Merge result files of the same machine, keeping the best run of each test.

usage: tools/merge-best.py OUT RUN RUN...

Copyright 2026 The Prismark Authors. Apache-2.0.

A test is chosen as a whole (its series, raw results and derived rows come from one run):
  - throughput series: highest steady perf; time series: lowest median
  - ISA-uplift pair (baseline + max): highest gain
  - realistic from-rest pair (warm + cold): lowest cold median
  - K9 wake-up test per core type: highest mean R_resp (with its calibration)
  - K10 timer per condition: lowest p99.9
Scaling and the ratios are then recomputed from the merged samples, as analysis_tp.c does.
"""
import json, random, statistics, sys, uuid

B = 2000
rng = random.Random(1)


def boot_ratio(a, b):
    a, b = [x for x in a if x == x], [x for x in b if x == x]
    est = statistics.median(a) / statistics.median(b)
    reps = sorted(statistics.median(rng.choices(a, k=len(a))) / statistics.median(rng.choices(b, k=len(b)))
                  for _ in range(B))
    return {"est": est, "lo": reps[int(0.025 * B)], "hi": reps[int(0.975 * B) - 1]}


def skey(s):
    return (s["kernel"], s.get("variant"), s["mode"], s["tier"], s.get("purpose"), s["core_type"], s["n"],
            s.get("start"), s.get("working_set_bytes"))


def rkey(r):
    p = r["params"]
    return (r["kernel"], r.get("variant"), r["mode"], r["tier"], p.get("purpose"), r["core"]["requested"],
            p.get("threads", p.get("instances", 1)), p.get("start"), p.get("working_set_bytes"))


def unit_of(k):
    kernel, variant, mode, tier, purpose, core, n, start, ws = k
    if purpose == "warmup":
        return ("warmup", mode)  # not scored: the first run's
    if purpose == "isa_uplift":
        return ("uplift", kernel, variant)
    if mode == "cold_burst" and kernel != "K9":
        return ("rest", kernel, core)
    if mode == "cold_burst":
        return ("K9", core)
    if mode == "periodic":
        return None  # by condition, below
    return ("series",) + k


def steady(r):
    return r["samples"][r["steady"]["from_window"]:] if "steady" in r else r["samples"]


runs = [json.load(open(f)) for f in sys.argv[2:]]
for d in runs[1:]:
    assert d["machine"]["cpu"]["model"] == runs[0]["machine"]["cpu"]["model"]

# Index every run by test unit.
idx = []
for d in runs:
    an = d["analysis"]
    u = {}
    for s in an["series"]:
        u.setdefault(unit_of(skey(s)), {"series": [], "results": []})["series"].append(s)
    for r in d["results"]:
        if r["mode"] == "periodic":
            key = ("K10", r["params"]["condition"])
        else:
            key = unit_of(rkey(r))
        u.setdefault(key, {"series": [], "results": []})["results"].append(r)
    for row in an["cold_burst"]:
        u.setdefault(("K9", row["core_type"]), {"series": [], "results": []}).setdefault("cb", []).append(row)
    for c in an["calibration"]:
        u[("K9", c["core_type"])]["calib"] = c
    for p in an["periodic"]:
        u.setdefault(("K10", p["condition"]), {"series": [], "results": []})["periodic"] = p
    ratios = {(x["name"], x["kernel"], x.get("variant"), x["core_type"]): x for x in an["ratios"]}
    for key, g in u.items():
        g["ratios"] = ratios
    idx.append(u)


def score(key, g):
    """Higher is better."""
    kind = key[0]
    if kind == "uplift":
        r = g["ratios"].get(("U_ISA", key[1], key[2], "P"))
        return r["value"]["est"] if r else None
    if kind == "rest":
        cold = [s for s in g["series"] if s.get("start") == "cold"]
        return -cold[0]["median"]["est"] if cold else None
    if kind == "K9":
        rows = g.get("cb", [])
        return statistics.mean(r["R_resp"]["est"] for r in rows) if rows else None
    if kind == "K10":
        return -g["periodic"]["p999_ns"] if "periodic" in g else None
    s = g["series"][0] if g["series"] else None
    if not s:
        return None
    if s.get("purpose") == "warmup":
        return 0
    if s.get("perf_steady"):
        return s["perf_steady"]["est"]
    return -s["median"]["est"]


out = json.loads(json.dumps(runs[0]))
an = out["analysis"]
keys = list(idx[0])
for u in idx[1:]:
    keys += [k for k in u if k not in keys]
chosen = {}
for key in keys:
    cands = [(score(key, u[key]), -i) for i, u in enumerate(idx) if key in u and score(key, u[key]) is not None]
    chosen[key] = -max(cands)[1] if cands else 0

series, results, cb, calib, periodic = [], [], [], [], []
for key in keys:
    g = idx[chosen[key]].get(key)
    if not g:
        continue
    series += g["series"]
    results += g["results"]
    cb += g.get("cb", [])
    if "calib" in g:
        calib.append(g["calib"])
    if "periodic" in g:
        periodic.append(g["periodic"])

# Keep each list in the first run's order.
def reorder(items, ref, keyf):
    pos = {keyf(x): i for i, x in enumerate(ref)}
    return sorted(items, key=lambda x: pos.get(keyf(x), len(pos)))

out["results"] = reorder(results, runs[0]["results"], lambda r: (r["mode"], json.dumps(rkey(r)), json.dumps(r["params"], sort_keys=True)))
an["series"] = reorder(series, runs[0]["analysis"]["series"], lambda s: skey(s))
an["cold_burst"] = reorder(cb, runs[0]["analysis"]["cold_burst"], lambda r: (r["core_type"], r["W_ms"]))
an["calibration"] = reorder(calib, runs[0]["analysis"]["calibration"], lambda c: c["core_type"])
an["periodic"] = reorder(periodic, runs[0]["analysis"]["periodic"], lambda p: p["condition"])

R = {rkey(r): r for r in out["results"] if r["params"].get("purpose") != "warmup"}

# Scaling against n = 1 (MC threaded).
scaling = []
for row in runs[0]["analysis"]["scaling"]:
    k, n = row["kernel"], row["n"]
    one, rn = R[(k, None, "mc_threaded", "baseline", None, "P", 1, None, None)], R[(k, None, "mc_threaded", "baseline", None, "P", n, None, None)]
    S = boot_ratio(steady(rn), steady(one)) if "steady" in rn else boot_ratio(one["samples"], rn["samples"])
    E = {x: S[x] / n for x in S}
    if n > 1:
        f = 1 / (1 - 1 / n)
        p = {x: (1 - 1 / S[x]) * f for x in S}
    else:
        S = E = {"est": 1, "lo": S["lo"], "hi": S["hi"]}
        p = {"est": None, "lo": None, "hi": None}
    scaling.append({"kernel": k, "n": n, "S": S, "E": E, "p": p})
an["scaling"] = scaling


def k1x_fraction(d):
    """K1x's share of the compile work, from a run's own R_build (it is not stored)."""
    rr = {rkey(r): r for r in d["results"]}
    for x in d["analysis"]["ratios"]:
        if x["name"] == "R_build":
            k1x = rr[("K1x", None, "mc_threaded", "baseline", None, "P", x["n"], None, None)]
            k1 = rr[("K1", None, "mc_threaded", "baseline", None, "P", x["n"], None, None)]
            inv = statistics.median(1 / w for w in steady(k1) if w > 0)
            return statistics.median(k1x["samples"]) / (x["value"]["est"] * 3600e9 * inv)


fr = [k1x_fraction(d) for d in runs]
assert max(fr) / min(fr) < 1.001, fr

ratios = []
for x in runs[0]["analysis"]["ratios"]:
    name, k, v, core = x["name"], x["kernel"], x.get("variant"), x["core_type"]
    if name == "R_build":
        k1x = R[("K1x", None, "mc_threaded", "baseline", None, "P", x["n"], None, None)]
        k1 = R[("K1", None, "mc_threaded", "baseline", None, "P", x["n"], None, None)]
        t = [fr[0] * 3600e9 / w for w in steady(k1) if w > 0]
        val = boot_ratio(k1x["samples"], t)
    elif name == "U_ISA":
        m = R[(k, v, "st_sustained", "max", "isa_uplift", core, 1, None, None)]
        b = R[(k, v, "st_sustained", "baseline", "isa_uplift", core, 1, None, None)]
        val = boot_ratio(steady(m), steady(b))
    else:  # R_resp: t_warm / t_cold
        w = R[(k, v, "cold_burst", "baseline", None, core, 1, "warm", None)]
        c = R[(k, v, "cold_burst", "baseline", None, core, 1, "cold", None)]
        val = boot_ratio(w["samples"], c["samples"])
    ratios.append(dict(x, value=val))
an["ratios"] = ratios

ids = [d["run_id"] for d in runs]
out["run_id"] = str(uuid.uuid5(uuid.NAMESPACE_URL, "prismark-merge:" + ",".join(ids)))
out["merged_from"] = ids
out["summary"] = runs[0]["summary"].split("\n")[1] + "\nBest of %d runs (%s), each test from the run where it scored best.\n" % (
    len(runs), ", ".join(i[:8] for i in ids))

src = {}
for key in keys:
    src.setdefault(ids[chosen[key]][:8], []).append(key)
for i, ks in src.items():
    print(i, len(ks), "tests")
json.dump(out, open(sys.argv[1], "w"), separators=(",", ":"))
