#!/usr/bin/env python3
"""The three readers of a result agree on the tests: the desktop app (apps/gui/metrics.cpp), `prismark compare`
(src/stats/headline.c) and the website (site/compare-data.js) list the same tests, each with the same unit and
the same direction (higher or lower is better), and read the same field of the analysis for it.

usage: test_catalogue.py SOURCE_DIR
Copyright 2026 The Prismark Authors. Apache-2.0.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(sys.argv.pop(1) if len(sys.argv) > 1 else Path(__file__).resolve().parent.parent)
STRING = r'"((?:[^"\\]|\\.)*)"'


def app():
    src = (ROOT / "apps/gui/metrics.cpp").read_text()
    src = src[src.index("static const QVector<Metric> m = {"):]
    starts = [m.start() for m in re.finditer(r'\{"(\w+)", "(?:mc|st|mem|resp|stress)",', src)]
    out = {}
    for i, s in enumerate(starts):
        chunk = src[s:starts[i + 1] if i + 1 < len(starts) else len(src)]
        lits = re.findall(STRING, chunk)
        tail = re.search(r',\s*(true|false),\s*\{"\w+"[^}]*\},\s*"(\w+)",\s*(\w+)\(', chunk)
        how = tail.group(3) if tail else "custom"
        out[lits[0]] = {"unit": lits[5], "higher": tail.group(1) == "true" if tail else
                        re.search(r',\s*(true|false),\s*\{"\w+"', chunk).group(1) == "true",
                        "kernel": tail.group(2) if tail else re.search(r'\},\s*"(\w+)",\s*\[', chunk).group(1),
                        "how": how}
    return out


def site():
    src = (ROOT / "site/compare-data.js").read_text()
    out = {}
    for m in re.finditer(r"\{ id: '(\w+)'.*?unit: '([^']*)', higher: (true|false), get: (\w+)", src):
        out[m.group(1)] = {"unit": m.group(2), "higher": m.group(3) == "true", "how": m.group(4)}
    return out


def cli():
    src = (ROOT / "src/stats/headline.c").read_text()
    out = {}
    for m in re.finditer(r'\{"(\w+)", "[^"]*", "[^"]*", "([^"]*)", (\d), (\w+), "(\w+)", (NULL|"\w+"), (NULL|"\w+"), '
                         r'([\de.-]+)\}', src):
        out[m.group(1)] = {"unit": m.group(2), "higher": m.group(3) == "1", "how": m.group(4),
                           "kernel": m.group(5)}
    return out


# The same reading rule under each reader's own name.
HOW = {
    "throughput": ("throughput", "throughput", "THROUGHPUT"),
    "full build": ("custom", "fullBuild", "K1X"),
    "median": ("medianOf", "medianOf", "MEDIAN"),
    "memory delay": ("k7", "memoryDelay", "K7"),
    "timer": ("periodic", "timer", "PERIODIC"),
    "wake-up": ("rresp", "wakeUp", "RRESP"),
    "stress score": ("stress", "stress", "STRESS"),
}


def rule(reader, name):
    for key, names in HOW.items():
        if names[reader] == name:
            return key
    return f"unknown ({name})"


class Catalogue(unittest.TestCase):
    def setUp(self):
        self.app, self.site, self.cli = app(), site(), cli()

    def test_parsed(self):
        for name, table in (("app", self.app), ("site", self.site), ("compare", self.cli)):
            self.assertGreaterEqual(len(table), 20, f"{name}: the list was not found or not parsed")

    def test_same_tests(self):
        self.assertEqual(set(self.app), set(self.site), "app and website")
        self.assertEqual(set(self.app), set(self.cli), "app and prismark compare")

    def test_same_unit_and_direction(self):
        for k, a in self.app.items():
            for name, other in (("website", self.site), ("prismark compare", self.cli)):
                if k not in other:
                    continue
                self.assertEqual(a["unit"].replace("µ", "u"), other[k]["unit"].replace("µ", "u"), f"{k}: unit, {name}")
                self.assertEqual(a["higher"], other[k]["higher"], f"{k}: which direction is better, {name}")

    def test_same_reading_rule(self):
        for k, a in self.app.items():
            want = rule(0, a["how"])
            if k in self.site:
                self.assertEqual(want, rule(1, self.site[k]["how"]), f"{k}: website reads it differently")
            if k in self.cli:
                self.assertEqual(want, rule(2, self.cli[k]["how"]), f"{k}: prismark compare reads it differently")
                self.assertEqual(a["kernel"], self.cli[k]["kernel"], f"{k}: kernel, prismark compare")

    def test_direction_follows_unit(self):
        # Times and delays are better lower; speeds and the wake-up percentage are better higher.
        lower = {"ms", "s", "ns", "us", "µs"}
        for k, a in self.app.items():
            self.assertEqual(a["higher"], a["unit"] not in lower, f"{k}: {a['unit']} but higher={a['higher']}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
