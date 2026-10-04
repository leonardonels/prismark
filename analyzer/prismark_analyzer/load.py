"""Result-file loading (schema prismark/1)."""
from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np


@dataclass
class Series:
    kernel: str
    mode: str
    tier: str
    unit: str
    params: dict
    core: dict
    samples: np.ndarray
    raw: dict = field(repr=False)

    @property
    def variant(self) -> str | None:
        return self.raw.get("variant")

    @property
    def windowed(self) -> bool:
        return "window_ms" in self.params

    @property
    def n(self) -> int:
        return int(self.params.get("threads", self.params.get("instances", 1)))

    @property
    def steady(self) -> np.ndarray:
        """Steady-state windows (sustained runs) or all samples."""
        if not self.windowed:
            return self.samples
        start = int(self.raw.get("steady", {}).get("from_window", 0))
        return self.samples[start:] if start < len(self.samples) else self.samples

    @property
    def label(self) -> str:
        v = f" {self.variant}" if self.variant else ""
        return f"{self.kernel}{v} {self.mode} n={self.n} {self.tier}"


@dataclass
class Run:
    path: Path
    doc: dict
    series: list[Series]

    @property
    def model(self) -> str:
        return self.doc.get("machine", {}).get("cpu", {}).get("model", "?")

    @property
    def analysis(self) -> dict:
        return self.doc.get("analysis", {})

    def select(self, kernel=None, mode=None, **params) -> list[Series]:
        out = []
        for s in self.series:
            if kernel and s.kernel != kernel:
                continue
            if mode and s.mode != mode:
                continue
            if any(s.params.get(k) != v for k, v in params.items()):
                continue
            out.append(s)
        return out


def load(path: str | Path) -> Run:
    path = Path(path)
    doc = json.loads(path.read_text())
    if doc.get("schema") != "prismark/1":
        raise ValueError(f"{path}: not a prismark/1 result file")
    series = []
    for r in doc.get("results", []):
        samples = np.array([np.nan if x is None else x for x in r.get("samples", [])], dtype=float)
        series.append(Series(r["kernel"], r["mode"], r.get("tier", "baseline"), r.get("unit", ""),
                             r.get("params", {}), r.get("core", {}), samples, r))
    return Run(path, doc, series)
