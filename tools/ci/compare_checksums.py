#!/usr/bin/env python3
"""
Gate 2: kernel output checksums must be identical across ISAs. Compares the
`prismark checksums -o FILE` reports of two or more builds. Every report must
be repeatable and internally consistent, and every
kernel/size present in all reports must have the same input hash and output
checksum everywhere. Kernels unavailable on a host (K1 without a snapshot)
are skipped, never estimated.

  tools/ci/compare_checksums.py x86_64.json aarch64.json
  tools/ci/compare_checksums.py --require K1 linux-x86_64.json linux-aarch64.json macos-arm64.json

--require KERNEL fails unless that kernel was measured in every report (the
compile-test gate: one compiler build in every package must give one output).

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
        if prev and prev != val:  # a kernel listed twice must agree with itself
            bad.append(f"{path}: {key} is listed twice with different values {prev} vs {val}")
        out[key] = val
    return doc.get("baseline_level"), out, bad


def main():
    args, required = sys.argv[1:], []
    while len(args) >= 2 and args[0] == "--require":
        required.append(args[1])
        args = args[2:]
    if len(args) < 2:
        sys.exit(__doc__)
    reports = [entries(p) for p in args]
    problems = [b for _, _, bad in reports for b in bad]
    for kernel in required:
        for path, (_, e, _) in zip(args, reports):
            if not any(key[0] == kernel for key in e):
                problems.append(f"{path}: {kernel} was not measured")
    common = set.intersection(*(set(e) for _, e, _ in reports))
    for key in sorted(common, key=str):
        vals = {e[key] for _, e, _ in reports}
        if len(vals) > 1:
            problems.append(f"{key}: " + "  ".join(f"{lvl}={e[key]}" for lvl, e, _ in reports))
    print(f"{len(common)} kernel/size combinations compared across {', '.join(str(l) for l, _, _ in reports)}")
    for p in problems:
        print("MISMATCH", p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
