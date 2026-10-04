#!/usr/bin/env python3
"""
Gate 2: kernel output checksums must be identical across ISAs. Compares the
`prismark checksums -o FILE` reports of two or more builds. Every report must
be repeatable and internally consistent (max tier == baseline), and every
kernel/size present in all reports must have the same input hash and output
checksum everywhere. Kernels unavailable on a host (K1 without a snapshot)
are skipped, never estimated.

  tools/ci/compare_checksums.py x86_64.json aarch64.json

Copyright 2026 The Prismark Authors. Apache-2.0.
"""
import json
import sys


def entries(path):
    doc = json.load(open(path))
    if doc.get("schema") != "prismark-checksums/1":
        sys.exit(f"{path}: not a checksum report")
    out, bad = {}, []
    for k in doc["kernels"]:
        if "unavailable" in k:
            continue
        key = (k["kernel"], k.get("variant"), k["size"])
        if not k.get("repeatable"):
            bad.append(f"{path}: {key} is not repeatable")
        prev = out.get(key)
        val = (k["input_hash"], k["checksum"])
        if prev and prev != val:  # the max tier must reproduce the baseline exactly
            bad.append(f"{path}: {key} differs between tiers {prev} vs {val}")
        out[key] = val
    return doc.get("baseline_level"), out, bad


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    reports = [entries(p) for p in sys.argv[1:]]
    problems = [b for _, _, bad in reports for b in bad]
    common = set.intersection(*(set(e) for _, e, _ in reports))
    for key in sorted(common, key=str):
        vals = {e[key] for _, e, _ in reports}
        if len(vals) > 1:
            problems.append(f"{key}: " + "  ".join(f"{lvl}={e[key]}" for lvl, e, _ in reports))
    print(f"{len(common)} kernel/size combinations compared across {', '.join(l for l, _, _ in reports)}")
    for p in problems:
        print("MISMATCH", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
