#!/usr/bin/env python3
"""Write the numbers the website shows, read from the code and from GitHub.

usage: tools/site_data.py OUT

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


def latest_version():
    request = urllib.request.Request(RELEASES)
    token = os.environ.get("GITHUB_TOKEN")
    if token:
        request.add_header("Authorization", f"Bearer {token}")
    with urllib.request.urlopen(request) as response:
        releases = json.load(response)    
    published = [r for r in releases if not r["draft"]]
    return published[0]["tag_name"].removeprefix("v")



def count_tests():
    source = (ROOT / "apps/cli/main.c").read_text()
    table = source.split("} ITEMS[] = {")[1].split("};")[0]
    return len(set(re.findall(r'^\s*\{\d, "[^"]*", "(\w+)"', table, re.MULTILINE)))


def count_measurements():
    source = (ROOT / "apps/gui/metrics.cpp").read_text()
    return len(re.findall(r'^\s*\{"\w+", "(?:mc|st|mem|resp|stress)",', source, re.MULTILINE))


data = {
    "version": latest_version(),
    "tests": count_tests(),
    "measurements": count_measurements(),
}
if not all(data.values()):
    sys.exit(f"site_data: could not read the tables: {data}")

pathlib.Path(sys.argv[1]).write_text(json.dumps(data) + "\n")
print(data)