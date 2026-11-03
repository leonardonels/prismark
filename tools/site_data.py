#!/usr/bin/env python3
"""Write what the website shows, read from the code, the references and GitHub.

usage: tools/site_data.py OUT

Writes OUT (the numbers) and references.json beside it: the reference results,
cut down to the fields site/compare-data.js reads to work out each test's value.

Copyright 2026 The Prismark Authors. Apache-2.0.
"""
import json
import os
import pathlib
import re
import sys
import urllib.request

ROOT = pathlib.Path(__file__).parent.parent
RELEASES = "https://api.github.com/repos/leonardonels/prismark/releases"


# What each download link on the homepage points at: the end of the file's name in a release.
PACKAGES = {
    "macos": "-macos-arm64.dmg",
    "windows": "-windows-x86_64.zip",
    "deb-x86_64": "_amd64.deb",
    "deb-arm64": "_arm64.deb",
    "rpm-x86_64": ".x86_64.rpm",
    "rpm-arm64": ".aarch64.rpm",
    "android": "-android-arm64-release.apk",
}


def latest_release():
    request = urllib.request.Request(RELEASES)
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request) as response:
        releases = json.load(response)    
    published = [r for r in releases if not r["draft"]]
    return published[0]


def downloads(release):
    """The address of each package, or the build stops: a link to nothing is worse than no new site."""
    assets = release["assets"]
    found = {}
    for key, ending in PACKAGES.items():
        urls = [a["browser_download_url"] for a in assets if a["name"].endswith(ending)]
        if len(urls) != 1:
            sys.exit(f"site_data: {release['tag_name']} has {len(urls)} files ending in {ending}")
        found[key] = urls[0]
    return found


def count_tests():
    source = (ROOT / "apps/cli/main.c").read_text()
    table = source.split("} ITEMS[] = {")[1].split("};")[0]
    return len(set(re.findall(r'^\s*\{\d, "[^"]*", "(\w+)"', table, re.MULTILINE)))


def count_measurements():
    source = (ROOT / "apps/gui/metrics.cpp").read_text()
    return len(re.findall(r'^\s*\{"\w+", "(?:mc|st|mem|resp|stress)",', source, re.MULTILINE))


def check_catalogue():
    """The site lists the same tests as the app, or the build stops."""
    app = (ROOT / "apps/gui/metrics.cpp").read_text()
    site = (ROOT / "site/compare-data.js").read_text()
    in_app = set(re.findall(r'^\s*\{"(\w+)", "(?:mc|st|mem|resp|stress)",', app, re.MULTILINE))
    in_site = set(re.findall(r"^\s*\{ id: '(\w+)', group:", site, re.MULTILINE))
    if in_app != in_site:
        sys.exit(f"site_data: tests differ from the app: {sorted(in_app ^ in_site)}")


def keep(row, *keys):
    return {key: row[key] for key in keys if key in row}


def reference(path):
    doc = json.loads(path.read_text())
    analysis = doc.get("analysis", {})
    machine = doc["machine"]
    cpu = machine["cpu"]
    return {
        "file": path.stem,
        "schema": doc["schema"],
        "run_id": doc["run_id"],
        **keep(doc, "name", "started_utc", "complete"),
        "config": keep(doc.get("config", {}), "quick", "sustained_max_s"),
        "harness": keep(doc.get("harness", {}), "version"),
        "machine": {
            **keep(machine, "os", "isa"),
            "cpu": {**keep(cpu, "model", "llc_bytes"), "cores": [keep(c, "type") for c in cpu.get("cores", [])]},
        },
        "state": {"start": keep(doc.get("state", {}).get("start", {}), "power_mode", "ac_online")},
        "analysis": {
            "series": [
                keep(r, "kernel", "mode", "tier", "purpose", "core_type", "n", "start", "working_set_bytes",
                     "perf_steady", "median", "steady_reached", "clamped_windows", "clamped_lowest_mhz")
                for r in analysis.get("series", [])
            ],
            "cold_burst": [keep(r, "W_ms", "core_type", "cell", "R_resp") for r in analysis.get("cold_burst", [])],
            "periodic": [keep(r, "condition", "p999_ns") for r in analysis.get("periodic", [])],
            "stress": [keep(r, "part", "score", "verdict", "checksum_errors", "wrong_tasks")
                       for r in analysis.get("stress", [])],
            "checksums": [keep(r, "consistent") for r in analysis.get("checksums", [])],
        },
    }


def references():
    return [reference(path) for path in sorted((ROOT / "references").glob("*.json"))]


check_catalogue()

release = latest_release()
data = {
    "version": release["tag_name"].removeprefix("v"),
    "downloads": downloads(release),
    "tests": count_tests(),
    "measurements": count_measurements(),
}
if not all(data.values()):
    sys.exit(f"site_data: could not read the tables: {data}")

refs = references()
if not refs:
    sys.exit("site_data: no reference results found")

out = pathlib.Path(sys.argv[1])
out.write_text(json.dumps(data) + "\n")
out.with_name("references.json").write_text(json.dumps(refs, separators=(",", ":")) + "\n")
print(data, f"{len(refs)} references")