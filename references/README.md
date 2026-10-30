# Reference systems

Every `.json` file in this folder is one reference system in the desktop
app's rankings. The files are built into the app, so after adding, editing or
removing one, rebuild (`cmake --build --preset linux-clang`).

- **Add a system:** add a file.
- **Remove a system:** delete its file.

The command line reads the same files (`prismark references`, `prismark
compare latest <file name without .json>`); they are installed to
`share/prismark/references` beside it.

Users can add their own references without rebuilding: the app also reads
`results/references` in their results folder (Menu › Data folders), next to
`results/runs` where their own runs are saved.

## Two kinds of file

**A real result.** A `prismark-*.json` written by a run on a reference
machine. Copy it here as it is: the app reads its values, its machine and its
instruction sets from it. Every system shipped here is one.

A result file may get a `"name"` at the top level, shown in the rankings and by
the command line in place of the processor's model. Use it when the model says
little, as with an engineering sample ("Genuine Intel(R) CPU 0000"); the
measured model stays in `machine.cpu.model`.

Several runs of one machine can be merged into one file that keeps, for each
test, the run where it scored best: `tools/merge-best.py OUT RUN RUN...`. It
also recomputes the scaling and the ratios from the merged samples. The runs
may be partial (a few tests each, as when the app runs one test): the merge
covers every test any of them has. The `intel-i9-14900k`, `intel-i9-10900k-es`
and `amd-ryzen5-7500f` references are each the best of two full runs, `apple-m2-macos`
the best of three;
`intel-i9-14900k-linux` is assembled from 14 runs of a few tests each
(`merged_from` lists their run IDs).

**A placeholder.** Example values, typed in by hand, for a ranking to compare
against when no real result exists (none are shipped). The app tags them PLACEHOLDER and shows a
banner while any are loaded. The format:

```json
{
  "schema": "prismark-reference/1",
  "placeholder": true,
  "name": "Desktop x86-64, 16 cores / 32 threads",
  "machine": {"os": "linux", "isa": "x86_64", "cpus": 32, "types": "32×P"},
  "results": {
    "mc_k2": {"value": 74, "uncertainty": 0.02},
    "j_idle": {"value": 41}
  }
}
```

| Field | Meaning |
| --- | --- |
| `name` | shown in the ranking |
| `machine.os` | `linux`, `windows`, `macos`, `android`, `ios` or `ipados` |
| `machine.isa` | `x86_64` or `aarch64` |
| `machine.cpus`, `machine.types` | thread count, and how they split by core type (P fast, E efficient) |
| `results` | one entry per test; leave out the tests the system cannot run |
| `value` | in the test's unit, below |
| `uncertainty` | optional; half the 95 % range as a fraction of the value (0.02 = ±2 %) |

## Test keys

| Key | Test | Unit |
| --- | --- | --- |
| `mc_k2` | 3D rendering on all cores | M samples/s |
| `mc_k1` | Compiling code on all cores | builds/h |
| `mc_k1x` | Full software build | s |
| `mc_k12` | Vector maths on all cores | GFLOP/s |
| `st_k2` | 3D rendering on one core | M samples/s |
| `st_k1` | Compiling code on one core | builds/h |
| `st_k12` | Vector maths on one core | GFLOP/s |
| `b_k3` | Compressing a file | ms |
| `b_k4` | Opening a photo | ms |
| `b_k5` | Reading structured data (JSON) | ms |
| `b_k6` | Starting a small script | ms |
| `m_lat1` | Memory delay | ns |
| `m_latn` | Memory delay while every core uses memory | ns |
| `j_idle` | Timer punctuality on an idle computer (worst 1 in 1000) | µs |
| `j_load` | Timer punctuality while other cores are busy | µs |
| `r_1ms` | Reacting from rest: a 1 ms task | % |
| `r_10ms` | Reacting from rest: a 10 ms task | % |
| `c_k4` | Opening a photo from rest | ms |
| `c_k6` | Starting a small script from rest | ms |
| `s_st` | One core under stress (10th minute) | GFLOP/s |
| `s_mc` | All cores under stress (10th minute) | GFLOP/s |
| `s_cc` | Each core in turn: the slowest core | GFLOP/s |
| `s_mem` | Memory under stress (10th minute) | GB/s |

A key the app does not know is reported when it starts (on the terminal), so
typos do not pass silently.

Result files measured before ISA uplift was removed still hold its series and
U_ISA ratios; the app and `prismark compare` ignore them, and
`tools/merge-best.py` still merges such files.
