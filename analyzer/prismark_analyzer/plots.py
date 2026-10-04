"""Plots of one run: responsiveness curve, perf(t), scaling, latency, jitter."""
from __future__ import annotations

from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
import numpy as np

from .load import Run


def _save(fig, out: Path, name: str) -> Path:
    p = out / f"{name}.png"
    fig.tight_layout()
    fig.savefig(p, dpi=130)
    plt.close(fig)
    return p


def responsiveness(run: Run, out: Path) -> Path | None:
    # Files from before ABI 4 may also hold series measured with changed power settings; keep "as configured".
    rows = [r for r in run.analysis.get("cold_burst", []) if r.get("cell", "as_shipped") == "as_shipped"]
    if not rows:
        return None
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for core in sorted({r["core_type"] for r in rows}):
        pts = sorted((r for r in rows if r["core_type"] == core), key=lambda r: r["W_ms"])
        w = [p["W_ms"] for p in pts]
        est = np.array([p["R_resp"]["est"] for p in pts], float)
        lo = np.array([p["R_resp"]["lo"] for p in pts], float)
        hi = np.array([p["R_resp"]["hi"] for p in pts], float)
        ax.errorbar(w, est, yerr=[est - lo, hi - est], marker="o", capsize=2, label=f"core {core}")
    ax.set_xscale("log")
    ax.set_xlabel("W (ms at f_max)")
    ax.set_ylabel("R_resp = t_warm / t_cold")
    ax.set_ylim(0, 1.05)
    ax.set_title("Responsiveness curve (K9)")
    ax.legend(fontsize=8)
    return _save(fig, out, "responsiveness")


def perf_t(run: Run, out: Path) -> list[Path]:
    paths = []
    for i, s in enumerate(x for x in run.series if x.windowed):
        win = s.params["window_ms"] / 1e3
        t = (np.arange(len(s.samples)) + 1) * win
        fig, ax = plt.subplots(figsize=(7, 3.6))
        ax.plot(t, s.samples, lw=1.2, label=f"perf ({s.unit})")
        start = int(s.raw.get("steady", {}).get("from_window", 0))
        if start < len(t):
            ax.axvspan(t[start] - win, t[-1], color="tab:green", alpha=0.08, label="steady set")
        ax.set_xlabel("s")
        ax.set_ylabel(s.unit)
        temps = np.array([np.nan if x is None else x for x in s.raw.get("temp_c", [])], float)
        if np.isfinite(temps).any():
            ax2 = ax.twinx()
            ax2.plot(t[: len(temps)], temps, color="tab:red", lw=0.8, alpha=0.7)
            ax2.set_ylabel("°C", color="tab:red")
        ax.set_title(s.label)
        ax.legend(fontsize=8, loc="lower left")
        paths.append(_save(fig, out, f"perf_t_{i:02d}_{s.kernel}_{s.mode}_n{s.n}_{s.tier}"))
    return paths


def scaling(run: Run, out: Path) -> Path | None:
    rows = run.analysis.get("scaling", [])
    if not rows:
        return None
    fig, ax = plt.subplots(figsize=(6, 4.5))
    nmax = max(r["n"] for r in rows)
    ax.plot([1, nmax], [1, nmax], color="grey", ls="--", lw=0.8, label="ideal")
    for k in sorted({r["kernel"] for r in rows}):
        pts = sorted((r for r in rows if r["kernel"] == k), key=lambda r: r["n"])
        n = [p["n"] for p in pts]
        est = np.array([p["S"]["est"] for p in pts])
        lo = np.array([p["S"]["lo"] for p in pts])
        hi = np.array([p["S"]["hi"] for p in pts])
        ax.errorbar(n, est, yerr=[est - lo, hi - est], marker="o", capsize=2, label=k)
    ax.set_xlabel("threads n")
    ax.set_ylabel("S(n) = T1 / Tn")
    ax.set_title("MC threaded scaling")
    ax.legend()
    return _save(fig, out, "scaling")


def latency(run: Run, out: Path) -> Path | None:
    rows = [r for r in run.analysis.get("series", []) if r["kernel"] == "K7"]
    if not rows:
        return None
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for n in sorted({r["n"] for r in rows}):
        pts = sorted((r for r in rows if r["n"] == n), key=lambda r: r["working_set_bytes"])
        ax.plot([p["working_set_bytes"] / 1024 for p in pts], [p["median"]["est"] for p in pts], marker="o",
                label=f"{n} cop{'y' if n == 1 else 'ies'}")
    llc = run.doc.get("machine", {}).get("cpu", {}).get("llc_bytes")
    if llc:
        ax.axvline(llc / 1024, color="grey", ls=":", lw=0.8, label="LLC")
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    ax.set_xlabel("working set (KiB)")
    ax.set_ylabel("ns per dependent load")
    ax.set_title("K7 latency under contention")
    ax.legend()
    return _save(fig, out, "latency")


def jitter(run: Run, out: Path) -> Path | None:
    ss = run.select(kernel="K10", mode="periodic")
    if not ss:
        return None
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for s in ss:
        x = np.sort(s.samples) / 1e3
        ccdf = 1.0 - np.arange(len(x)) / len(x)
        ax.step(x, ccdf, where="post", label=s.params.get("condition"))
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlabel("wake-up lateness L (µs)")
    ax.set_ylabel("P(L > x)")
    ax.set_title("Periodic (K10) lateness, complementary CDF")
    ax.legend()
    return _save(fig, out, "jitter")


def all_plots(run: Run, out: Path) -> list[Path]:
    out.mkdir(parents=True, exist_ok=True)
    paths = [responsiveness(run, out), scaling(run, out), latency(run, out), jitter(run, out)]
    paths += perf_t(run, out)
    return [p for p in paths if p]
