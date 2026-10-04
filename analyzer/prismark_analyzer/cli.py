"""prismark-analyze: summaries, plots and multi-run tables of result files."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

from .load import load
from .stats import boot_median, boot_ratio, tail


def cmd_summary(args) -> int:
    for path in args.files:
        run = load(path)
        print(f"{path}: {run.model}, {len(run.series)} series, complete={run.doc.get('complete')}")
        for u in run.doc.get("unavailable", []):
            print(f"  unavailable: {u['kernel']} {u['mode']}: {u['reason']}")
        for s in run.series:
            if s.mode == "periodic":
                t = tail(s.samples)
                print(f"  {s.label:44s} p50 {t['p50'] / 1e3:8.1f} us  p99.9 {t['p999'] / 1e3:8.1f} us  max {t['max'] / 1e3:8.1f} us")
            else:
                est, lo, hi = boot_median(s.steady)
                print(f"  {s.label:44s} {est:12.5g} [{lo:.5g}, {hi:.5g}] {'' if s.windowed else s.unit}{s.unit if s.windowed else ''}")
    return 0


def cmd_plot(args) -> int:
    run = load(args.file)
    out = Path(args.out or Path(args.file).with_suffix(""))
    from .plots import all_plots

    for p in all_plots(run, out):
        print(p)
    return 0


def cmd_check(args) -> int:
    """Recomputes the core's medians from raw samples; differences beyond 1e-9 relative are reported."""
    run = load(args.file)
    bad = 0
    for row in run.analysis.get("series", []):
        if "median" not in row:
            continue
        match = [s for s in run.series if s.kernel == row["kernel"] and s.mode == row["mode"]
                 and s.n == row["n"] and s.tier == row["tier"] and s.core.get("requested") == row["core_type"]
                 and s.params.get("working_set_bytes") == row.get("working_set_bytes")
                 and s.params.get("start") == row.get("start")]
        for s in match[:1]:
            m = float(np.median(s.samples))
            if abs(m - row["median"]["est"]) > 1e-9 * abs(m):
                print(f"mismatch {s.label}: core {row['median']['est']} analyzer {m}")
                bad += 1
    print("medians agree with the core" if not bad else f"{bad} mismatches")
    return 1 if bad else 0


def cmd_table(args) -> int:
    """Kernel x run table of medians (steady windows or times) for multi-run research."""
    runs = [load(p) for p in args.files]
    keys = []
    for r in runs:
        for s in r.series:
            if s.mode != "periodic" and s.label not in keys:
                keys.append(s.label)
    print("series," + ",".join(f'"{r.model} ({r.path.name})"' for r in runs))
    for k in keys:
        cells = []
        for r in runs:
            s = next((x for x in r.series if x.label == k), None)
            cells.append(f"{np.median(s.steady):.6g}" if s is not None and len(s.steady) else "")
        print(f'"{k}",' + ",".join(cells))
    return 0


def cmd_ratio(args) -> int:
    a, b = load(args.a), load(args.b)
    for s in a.series:
        t = next((x for x in b.series if x.label == s.label and x.params == s.params), None)
        if t is None or s.mode == "periodic":
            continue
        est, lo, hi = boot_ratio(s.steady, t.steady) if s.windowed else boot_ratio(t.steady, s.steady)
        flag = " *" if lo > 1 or hi < 1 else ""
        print(f"{s.label:44s} r = {est:.3f} [{lo:.3f}, {hi:.3f}]{flag}")
    return 0


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="prismark-analyze", description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("summary", help="medians with bootstrap CIs per series")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_summary)
    p = sub.add_parser("plot", help="PNG plots of one run")
    p.add_argument("file")
    p.add_argument("-o", "--out", help="output directory (default: next to the file)")
    p.set_defaults(fn=cmd_plot)
    p = sub.add_parser("check", help="cross-check the core's medians against the raw samples")
    p.add_argument("file")
    p.set_defaults(fn=cmd_check)
    p = sub.add_parser("table", help="CSV of medians, one column per run")
    p.add_argument("files", nargs="+")
    p.set_defaults(fn=cmd_table)
    p = sub.add_parser("ratio", help="per-series speed ratios A/B (research; the core's compare is normative)")
    p.add_argument("a")
    p.add_argument("b")
    p.set_defaults(fn=cmd_ratio)
    args = ap.parse_args(argv)
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())
