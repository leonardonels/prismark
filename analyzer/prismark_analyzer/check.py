"""Recomputes the core's point estimates (the "analysis" section) from the raw samples of the same file.

Only what the raw samples determine is checked: medians and quantiles, the steady-state and initial windows, the
ratios built from them, and that every 95 % interval is ordered. Bootstrap intervals themselves depend on the
core's random stream and are not recomputed. The JSON holds numbers to 10 significant digits, so values are
compared with a relative tolerance a little above that.
"""
from __future__ import annotations

import math

import numpy as np

from .load import Run, Series

INITIAL_WINDOWS = 3  # src/stats/analysis_tp.c
SCORE_FROM_S, SCORE_TO_S = 540.0, 600.0  # src/engine/ctx.h: the stress test's score is its 10th minute
REL_TOL = 5e-9


def _close(a, b) -> bool:
    if a is None or b is None:
        return a is None and b is None
    a, b = float(a), float(b)
    if math.isnan(a) or math.isnan(b):
        return math.isnan(a) and math.isnan(b)
    return abs(a - b) <= REL_TOL * max(abs(a), abs(b)) or a == b


def _median(x) -> float | None:
    x = np.asarray(x, float)
    return float(np.median(x)) if len(x) else None


def _initial(s: Series) -> np.ndarray:
    n = len(s.samples)
    start = 1 if n > 1 + INITIAL_WINDOWS else 0
    return s.samples[start:start + min(INITIAL_WINDOWS, n - start)]


def _steady(s: Series) -> np.ndarray:
    start = int(s.raw.get("steady", {}).get("from_window", 0))
    return s.samples[start:]


def _key(s: Series) -> tuple:
    return (s.kernel, s.variant, s.mode, s.tier, s.params.get("purpose"), s.n,
            s.params.get("working_set_bytes"), s.params.get("start"))


def _row_key(row: dict) -> tuple:
    return (row["kernel"], row.get("variant"), row["mode"], row["tier"], row.get("purpose"), row["n"],
            row.get("working_set_bytes"), row.get("start"))


class Checker:
    def __init__(self, run: Run):
        self.run = run
        self.problems: list[str] = []
        self.checked = 0
        # Series as the analysis counts them: the ones with samples, from the configuration the core analyses.
        self.series = [s for s in run.series if len(s.samples) and s.params.get("grid_cell", "as_shipped") == "as_shipped"]

    def value(self, what: str, core, mine) -> None:
        self.checked += 1
        if not _close(core, mine):
            self.problems.append(f"{what}: core {core}, recomputed {mine}")

    def interval(self, what: str, ci: dict | None) -> None:
        if not ci or ci.get("lo") is None or ci.get("hi") is None:
            return
        self.checked += 1
        if not ci["lo"] <= ci["hi"]:
            self.problems.append(f"{what}: interval [{ci['lo']}, {ci['hi']}] is reversed")

    def candidates(self, row: dict) -> list[Series]:
        found = [s for s in self.series if _key(s) == _row_key(row)]
        # Several series can share every key (one per core type); the core type then tells them apart.
        typed = [s for s in found if s.core.get("requested") in (None, row.get("core_type"))]
        return typed or found

    def est(self, row: dict, key: str):
        return (row.get(key) or {}).get("est")

    def series_rows(self) -> None:
        for row in self.run.analysis.get("series", []):
            label = f"{row['kernel']} {row['mode']} n={row['n']} {row['tier']}" + (
                f" {row['start']}" if row.get("start") else "") + (
                f" ws={row['working_set_bytes']}" if row.get("working_set_bytes") else "")
            cands = self.candidates(row)
            if not cands:
                self.problems.append(f"{label}: no raw series matches this analysis row")
                continue
            for key in ("median", "perf", "perf_steady", "perf_initial", "R_throttle", "job_ns"):
                self.interval(f"{label} {key}", row.get(key))
            if "perf_steady" in row:
                checks = {
                    "perf_steady": lambda s: _median(_steady(s)),
                    "perf_initial": lambda s: _median(_initial(s)),
                    "R_throttle": lambda s: _median(_steady(s)) / _median(_initial(s)),
                    "job_ns": lambda s: _median(s.raw.get("job_ns", [])),
                }
            else:
                checks = {"median": lambda s: _median(s.samples)}
            for key, f in checks.items():
                core = self.est(row, key)
                mine = [f(s) for s in cands]
                if any(_close(core, m) for m in mine):
                    self.checked += 1
                else:
                    self.value(f"{label} {key}", core, mine[0])

    def cold_burst(self) -> None:
        k9 = [s for s in self.series if s.kernel == "K9" and s.mode == "cold_burst"]
        for row in self.run.analysis.get("cold_burst", []):
            if row.get("cell", "as_shipped") != "as_shipped":
                continue
            w = row["W_ms"]
            warm = [s for s in k9 if s.params.get("W_ms") == w and s.params.get("start") == "warm"]
            cold = [s for s in k9 if s.params.get("W_ms") == w and s.params.get("start") == "cold"]
            warm = [s for s in warm if s.core.get("requested") in (None, row["core_type"])] or warm
            cold = [s for s in cold if s.core.get("requested") in (None, row["core_type"])] or cold
            label = f"K9 cold_burst W={w} ms {row['core_type']}"
            if not warm or not cold:
                self.problems.append(f"{label}: no raw warm and cold series")
                continue
            tw, tc = _median(warm[0].samples), _median(cold[0].samples)
            self.value(f"{label} t_warm_ns", self.est(row, "t_warm_ns"), tw)
            self.value(f"{label} t_cold_ns", self.est(row, "t_cold_ns"), tc)
            self.value(f"{label} R_resp", self.est(row, "R_resp"), tw / tc)
            if "wake_ns" in cold[0].raw:
                self.value(f"{label} wake_ns_median", row.get("wake_ns_median"), _median(cold[0].raw["wake_ns"]))
            for key in ("t_warm_ns", "t_cold_ns", "R_resp"):
                self.interval(f"{label} {key}", row.get(key))

    def periodic(self) -> None:
        raw = [s for s in self.series if s.mode == "periodic"]
        for row in self.run.analysis.get("periodic", []):
            match = [s for s in raw if s.params.get("condition") == row["condition"]]
            label = f"K10 periodic {row['condition']}"
            if not match:
                self.problems.append(f"{label}: no raw series")
                continue
            x = match[0].samples
            for key, q in (("p50_ns", 0.5), ("p99_ns", 0.99), ("p999_ns", 0.999)):
                self.value(f"{label} {key}", row.get(key), float(np.quantile(x, q)))  # type 7, as the core
            self.value(f"{label} max_ns", row.get("max_ns"), float(np.max(x)))
            self.value(f"{label} n", row.get("n"), len(x))

    def scaling(self) -> None:
        for row in self.run.analysis.get("scaling", []):
            label = f"{row['kernel']} scaling n={row['n']}"
            pick = lambda n: [s for s in self.series if s.kernel == row["kernel"] and s.mode == "mc_threaded"
                              and s.n == n and not s.params.get("purpose")]
            one, many = pick(1), pick(row["n"])
            if not one or not many:
                self.problems.append(f"{label}: no raw series for n=1 and n={row['n']}")
                continue
            a, b = one[0], many[0]
            S = _median(_steady(b)) / _median(_steady(a)) if b.windowed else _median(a.samples) / _median(b.samples)
            self.value(f"{label} S", self.est(row, "S"), S)
            self.value(f"{label} E", self.est(row, "E"), S / row["n"])
            if row["n"] > 1:
                self.value(f"{label} p", self.est(row, "p"), (1 - 1 / S) / (1 - 1 / row["n"]))
            for key in ("S", "E", "p"):
                self.interval(f"{label} {key}", row.get(key))

    def ratios(self) -> None:
        for row in self.run.analysis.get("ratios", []):
            if row["name"] != "R_resp":  # R_build needs the K1 work fraction; its interval is still checked
                self.interval(f"{row['name']} {row['kernel']}", row.get("value"))
                continue
            label = f"R_resp {row['kernel']} {row.get('core_type')}"
            pick = lambda start: [s for s in self.series if s.kernel == row["kernel"] and s.mode == "cold_burst"
                                  and s.params.get("start") == start
                                  and s.core.get("requested") in (None, row.get("core_type"))]
            warm, cold = pick("warm"), pick("cold")
            if not warm or not cold:
                self.problems.append(f"{label}: no raw warm and cold series")
                continue
            mine = [_median(w.samples) / _median(c.samples) for w in warm for c in cold]
            core = self.est(row, "value")
            if any(_close(core, m) for m in mine):
                self.checked += 1
            else:
                self.value(label, core, mine[0])
            self.interval(label, row.get("value"))

    def stress(self) -> None:
        raw = [s for s in self.series if s.mode == "stress"]
        for row in self.run.analysis.get("stress", []):
            part = [s for s in raw if s.params.get("part") == row["part"]]
            label = f"stress {row['part']}"
            if not part:
                self.problems.append(f"{label}: no raw series")
                continue
            for key in ("score", "first_minute", "last_minute", "hold_up"):
                self.interval(f"{label} {key}", row.get(key))
            if row["part"] == "each_core":  # the slowest core's median, once the part ran 10 minutes
                ran = sum(len(s.samples) * s.params["window_ms"] / 1e3 for s in part)
                slowest = min(_median(s.samples) for s in part)
                self.value(f"{label} score", self.est(row, "score"), slowest if ran >= SCORE_TO_S - 1e-6 else None)
                continue
            x, w = part[0].samples, part[0].params["window_ms"] / 1e3
            n = len(x)

            def windows(lo, hi):  # the windows lying within [lo, hi] seconds of the part
                a, b = math.ceil(lo / w - 1e-9), min(math.floor(hi / w + 1e-9), n)
                return a, max(b - a, 0)

            a, k = windows(SCORE_FROM_S, SCORE_TO_S)
            score = _median(x[a:a + k]) if n * w >= SCORE_TO_S - 1e-6 and k else None
            self.value(f"{label} score", self.est(row, "score"), score)
            a, k = windows(w, 60)
            if not k:
                a, k = 0, n
            last = min(n, int(60 / w + 0.5))
            first, end = _median(x[a:a + k]), _median(x[n - last:])
            self.value(f"{label} first_minute", self.est(row, "first_minute"), first)
            self.value(f"{label} last_minute", self.est(row, "last_minute"), end)
            self.value(f"{label} hold_up", self.est(row, "hold_up"), end / first)

    def all(self) -> "Checker":
        self.series_rows()
        self.cold_burst()
        self.periodic()
        self.scaling()
        self.ratios()
        self.stress()
        return self


def check(run: Run) -> Checker:
    return Checker(run).all()
