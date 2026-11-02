"""prismark-analyze check: every shipped reference agrees with its raw samples, and a wrong value is caught.

Run from analyzer/: uv run python -m unittest discover tests
"""
import copy
import json
import tempfile
import unittest
from pathlib import Path

from prismark_analyzer.check import check
from prismark_analyzer.load import load

REFERENCES = sorted((Path(__file__).resolve().parents[2] / "references").glob("*.json"))


def mutated(path: Path, change) -> Path:
    doc = json.loads(path.read_text())
    change(doc["analysis"])
    out = Path(tempfile.mkdtemp()) / path.name
    out.write_text(json.dumps(doc))
    return out


class Check(unittest.TestCase):
    def test_references_agree(self):
        self.assertTrue(REFERENCES)
        for path in REFERENCES:
            c = check(load(path))
            self.assertEqual(c.problems, [], path.name)
            self.assertGreater(c.checked, 100, path.name)

    def test_wrong_values_are_caught(self):
        path = next(p for p in REFERENCES if p.name == "intel-i9-14900k-linux.json")

        def first(rows, key):
            return next(r for r in rows if key in r)

        changes = {
            "median": lambda an: first(an["series"], "median")["median"].__setitem__("est", first(an["series"], "median")["median"]["est"] * 1.001),
            "R_throttle": lambda an: first(an["series"], "R_throttle")["R_throttle"].__setitem__("est", 9.0),
            "R_resp": lambda an: an["cold_burst"][0]["R_resp"].__setitem__("est", 0.5),
            "p999": lambda an: an["periodic"][0].__setitem__("p999_ns", an["periodic"][0]["p999_ns"] + 1),
            "scaling": lambda an: an["scaling"][-1]["S"].__setitem__("est", an["scaling"][-1]["S"]["est"] * 1.01),
            "reversed interval": lambda an: an["scaling"][-1]["S"].update(lo=2.0, hi=1.0),
        }
        for name, change in changes.items():
            c = check(load(mutated(path, change)))
            self.assertEqual(len(c.problems), 1, f"{name}: {c.problems}")


if __name__ == "__main__":
    unittest.main()
