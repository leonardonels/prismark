"""Bootstrap statistics matching the core's definitions (percentile intervals, 95 %)."""
from __future__ import annotations

import numpy as np

B = 2000


def boot_median(x, b: int = B, rng=None) -> tuple[float, float, float]:
    x = np.asarray(x, float)
    rng = rng or np.random.default_rng(0)
    if len(x) == 0:
        return (np.nan, np.nan, np.nan)
    reps = np.median(rng.choice(x, size=(b, len(x)), replace=True), axis=1)
    return float(np.median(x)), float(np.quantile(reps, 0.025)), float(np.quantile(reps, 0.975))


def boot_ratio(a, b_, b: int = B, rng=None) -> tuple[float, float, float]:
    """CI of median(a) / median(b_), resampling both sides independently."""
    a, b_ = np.asarray(a, float), np.asarray(b_, float)
    rng = rng or np.random.default_rng(0)
    ra = np.median(rng.choice(a, size=(b, len(a)), replace=True), axis=1)
    rb = np.median(rng.choice(b_, size=(b, len(b_)), replace=True), axis=1)
    reps = ra / rb
    return float(np.median(a) / np.median(b_)), float(np.quantile(reps, 0.025)), float(np.quantile(reps, 0.975))


def tail(x) -> dict:
    x = np.asarray(x, float)
    return {"p50": float(np.quantile(x, 0.5)), "p99": float(np.quantile(x, 0.99)),
            "p999": float(np.quantile(x, 0.999)), "max": float(np.max(x)), "n": int(len(x))}
