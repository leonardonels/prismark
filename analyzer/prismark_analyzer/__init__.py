"""Prismark analyzer: loads result files, recomputes statistics from raw samples, plots.

The C core computes every reported number; this package exists for multi-run
research and plotting, and as an independent cross-check of the core.
"""
from .load import Run, Series, load
from .stats import boot_median, boot_ratio

__all__ = ["Run", "Series", "load", "boot_median", "boot_ratio"]
