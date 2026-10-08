#!/usr/bin/env python3
"""
Command-line tests: what `prismark` prints and the exit status it returns, for the commands, the checks on
options, and one short run with list, show, compare and saving it as a reference. Every test runs with its own
data folder (XDG_DATA_HOME, HOME), so the user's results are never touched.

    tests/test_cli.py PATH/TO/prismark [SOURCE_DIR]

The bundled reference systems come from SOURCE_DIR/references (compiled into the program for builds run from
the build folder). Takes about 15 s: one short run, one stopped with Ctrl+C, one refused as too busy.

Copyright 2026 The Prismark Authors. Apache-2.0.
"""
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest

PRISMARK = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else "prismark"
SOURCE = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), "..")
REFS = os.path.join(SOURCE, "references")

# A run that needs no warm-up and no machine checks: one short test, a few seconds.
SHORT_RUN = ["run", "--quick", "--skip-preflight", "--tests", "json", "--mode", "short-task"]


class Cli(unittest.TestCase):
    def setUp(self):
        self.home = tempfile.mkdtemp(prefix="prismark-cli-")
        self.env = dict(os.environ, HOME=self.home, XDG_DATA_HOME=os.path.join(self.home, "data"), NO_COLOR="1")
        self.env.pop("SUDO_UID", None)
        self.env.pop("PKEXEC_UID", None)

    def tearDown(self):
        shutil.rmtree(self.home, ignore_errors=True)

    def run_cli(self, *args, code=0, timeout=120):
        p = subprocess.run([PRISMARK, *args], env=self.env, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                           timeout=timeout)
        self.assertEqual(p.returncode, code, f"prismark {' '.join(args)}\n--- stdout\n{p.stdout}\n--- stderr\n{p.stderr}")
        return p.stdout, p.stderr

    def runs_dir(self):
        return os.path.join(self.home, "data", "prismark", "results", "runs")

    def refs_dir(self):
        return os.path.join(self.home, "data", "prismark", "results", "references")

    # ---------- help and usage ----------

    def test_help(self):
        out, _ = self.run_cli("help")
        self.assertIn("usage:", out)
        self.assertIn("Exit status", out)
        out, _ = self.run_cli("help", "run")
        self.assertIn("--quick", out)
        self.assertIn("help advanced", out)
        out, _ = self.run_cli("help", "advanced")
        self.assertIn("--warmup", out)
        out, _ = self.run_cli("run", "--help")
        self.assertIn("--tests", out)
        out, _ = self.run_cli("help", "references")
        self.assertIn("references add RUN NAME", out)

    def test_no_arguments_off_a_terminal(self):
        _, err = self.run_cli(code=2)
        self.assertIn("nothing to do", err)

    def test_unknown(self):
        _, err = self.run_cli("frobnicate", code=2)
        self.assertIn("unknown command 'frobnicate'", err)
        _, err = self.run_cli("--bogus", code=2)
        self.assertIn("unknown option '--bogus'", err)
        _, err = self.run_cli("run", "--tests", "nope", code=2)
        self.assertIn("unknown test 'nope'", err)
        _, err = self.run_cli("run", "--mode", "nope", code=2)
        self.assertIn("unknown mode 'nope'", err)

    def test_tests_lists_names(self):
        out, _ = self.run_cli("tests")
        for name in ("Opening a photo", "--tests photo --mode short-task", "Timer punctuality", "from-rest"):
            self.assertIn(name, out)

    # ---------- checks before a run ----------

    def test_numbers_are_checked(self):
        for args, message in [
            (["--cpu", "abc"], "--cpu takes a whole number"),
            (["--cpu", "65000"], "no CPU 65000"),
            (["--max-threads", "-3"], "--max-threads takes a whole number"),
            (["--warmup", "x"], "--warmup takes a number"),
            (["--measure", "0"], "--measure takes a number above 0"),
            (["--seed", "12z"], "--seed takes a whole number"),
            (["--max-load", "2"], "--max-load is a fraction"),
            (["--min-reps", "50", "--max-reps", "20"], "--min-reps (50) is above"),
            (["--cpu"], "--cpu needs a value"),
        ]:
            with self.subTest(args=args):
                _, err = self.run_cli("run", *args, code=2)
                self.assertIn(message, err)

    def test_tests_and_modes_that_do_not_go_together(self):
        _, err = self.run_cli("run", "--tests", "photo", "--mode", "all-cores", code=2)
        self.assertIn("Opening a photo does not run in --mode all-cores", err)
        self.assertIn("nothing to measure", err)
        _, err = self.run_cli("run", "--tests", "json", "--mode", "timer", code=2)
        self.assertIn("nothing to measure", err)
        _, err = self.run_cli("run", "--tests", "matrix", "--mode", "warmed-up", "--no-isa-uplift", code=2)
        self.assertIn("only for the new-instruction gain", err)

    # ---------- results folder ----------

    def test_list_without_runs(self):
        out, _ = self.run_cli("list")
        self.assertIn("No runs", out)
        self.assertNotIn("prismark (a full run)", out)
        out, _ = self.run_cli("list", "--ids")
        self.assertEqual(out, "")
        _, err = self.run_cli("list", "--dir", os.path.join(self.home, "missing"), code=1)
        self.assertIn("no such folder", err)
        _, err = self.run_cli("show", "latest", code=1)
        self.assertIn("no result file, run or reference system 'latest'", err)

    def test_references(self):
        out, _ = self.run_cli("references", "--ids")
        self.assertIn("intel-i5-1035g1", out.split())
        out, _ = self.run_cli("references")
        self.assertIn("placeholder", out)
        _, err = self.run_cli("references", "remove", "intel-i5-1035g1", code=1)
        self.assertIn("comes with Prismark", err)
        _, err = self.run_cli("references", "add", "latest", code=2)
        self.assertIn("needs a run and a name", err)

    # ---------- compare ----------

    def test_compare_fixed_results(self):
        a, b = os.path.join(REFS, "amd-ryzen7-8845hs.json"), os.path.join(REFS, "intel-i5-1035g1.json")
        out, _ = self.run_cli("compare", a, b)
        rows = {line[:47].strip(): line[47:] for line in out.splitlines() if line.startswith("  ") and len(line) > 47}
        self.assertTrue(rows["3D rendering on all cores"].endswith("4.16x better"), rows["3D rendering on all cores"])
        self.assertTrue(rows["Reacting from rest: a 1 ms task"].endswith("33.5 pts worse"))
        self.assertIn("plugged in, run b3391174", out)
        self.assertIn("on battery, run 28e98b8f", out)
        self.assertIn("Percentages are compared by their difference in points", out)
        out, _ = self.run_cli("compare", a, a)
        self.assertIn("A and B are the same run", out)
        out, _ = self.run_cli("compare", a, "mini-4c")
        self.assertIn("placeholder holds example values", out)
        _, err = self.run_cli("compare", a, code=2)
        self.assertIn("compare needs two runs", err)

    # ---------- a run, and what follows it ----------

    def test_run_list_show_compare_save(self):
        _, err = self.run_cli(*SHORT_RUN[:-2], "--tests", "json,render", "--mode", "short-task")
        self.assertIn("3D rendering does not run in --mode short-task", err)
        self.assertIn("Finished in", err)
        files = os.listdir(self.runs_dir())
        self.assertEqual(len(files), 1, files)

        out, _ = self.run_cli("list", "--ids")
        run_id = out.split()[0]
        out, _ = self.run_cli("list")
        self.assertIn(run_id[:8], out)
        self.assertIn("times are local", out)
        out, _ = self.run_cli("show", run_id[:8])
        self.assertIn("Reading JSON", out)
        self.assertNotIn("(K5)", out)
        out, _ = self.run_cli("compare", "latest", "intel-i5-1035g1")
        self.assertIn("Reading structured data (JSON)", out)
        self.assertIn("A is a quick run", out)

        out, _ = self.run_cli("references", "add", "latest", "my-laptop")
        self.assertIn("as the reference system 'my-laptop'", out)
        self.assertTrue(os.path.exists(os.path.join(self.refs_dir(), "my-laptop.json")))
        _, err = self.run_cli("references", "add", "latest", "my-laptop", code=1)
        self.assertIn("--force replaces it", err)
        self.run_cli("references", "add", "latest", "my-laptop", "--force")
        _, err = self.run_cli("references", "add", "latest", "no/slashes", code=2)
        self.assertIn("cannot be a reference name", err)
        out, _ = self.run_cli("references")
        self.assertIn("(yours)", out)
        out, _ = self.run_cli("compare", "latest", "my-laptop")
        self.assertIn("A and B are the same run", out)
        out, _ = self.run_cli("references", "remove", "my-laptop")
        self.assertIn("Removed", out)
        self.assertFalse(os.path.exists(os.path.join(self.refs_dir(), "my-laptop.json")))

    # ---------- exit status ----------

    def test_ctrl_c_exits_130_and_keeps_the_result(self):
        out = os.path.join(self.home, "cancelled.json")
        p = subprocess.Popen([PRISMARK, "run", "--quick", "--skip-preflight", "--tests", "timer", "--mode", "timer",
                              "--periodic-seconds", "20", "-o", out], env=self.env, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        time.sleep(2)
        p.send_signal(signal.SIGINT)
        _, err = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 130, err)
        self.assertIn("Cancelled", err)
        self.assertTrue(os.path.exists(out))

    def test_too_busy_exits_3(self):
        _, err = self.run_cli("run", "--quick", "--max-load", "0", "--tests", "json", "--mode", "short-task",
                              "-o", os.path.join(self.home, "busy.json"), code=3)
        self.assertIn("background load", err)

    # ---------- completion ----------

    def test_completion(self):
        out, _ = self.run_cli("completion", "bash")
        self.assertIn("complete -F _prismark prismark", out)
        self.assertIn("compression", out)
        if shutil.which("bash"):
            subprocess.run(["bash", "-n"], input=out, text=True, check=True)
        out, _ = self.run_cli("completion", "zsh")
        self.assertIn("bashcompinit", out)
        self.run_cli("completion", "fish", code=2)


if __name__ == "__main__":
    unittest.main(argv=sys.argv[:1], verbosity=2)
