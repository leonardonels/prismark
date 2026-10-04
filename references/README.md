# Reference systems

Every `.json` file in this folder is one reference system in the desktop
app's rankings. The files are built into the app, so after adding, editing or
removing one, rebuild (`cmake --build --preset linux-clang`).

- **Add a system:** add a file.
- **Remove a system:** delete its file.

Users can add their own references without rebuilding: the app also reads
`results/references` in their results folder (Menu › Data folders), next to
`results/runs` where their own runs are saved.

## Two kinds of file

**A real result.** A `prismark-*.json` written by a run on a reference
machine. Copy it here as it is: the app reads its values, its machine and its
instruction sets from it. This is what references should eventually be.

**A placeholder.** Example values, typed in by hand, for a ranking to compare
against until real results exist. The app tags them PLACEHOLDER and shows a
banner while any are loaded. The format:

```json
{
  "schema": "prismark-reference/1",
  "placeholder": true,
  "name": "Desktop x86-64, 16 cores / 32 threads",
  "machine": {"os": "linux", "isa": "x86_64", "cpus": 32, "types": "32×P"},
  "newest_instructions": "x86-64-v4",
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
| `newest_instructions` | newest instruction set the system supports: `x86-64-v3` (AVX2), `x86-64-v4` (AVX-512), `armv8.2-a+dotprod+fp16` or `armv9-a+sve2`; leave it out if none |
| `results` | one entry per test; leave out the tests the system cannot run |
| `value` | in the test's unit, below |
| `uncertainty` | optional; half the 95 % range as a fraction of the value (0.02 = ±2 %) |

## Test keys

| Key | Test | Unit |
| --- | --- | --- |
| `mc_k2` | 3D rendering on all cores | M samples/s |
| `mc_k1` | Compiling code on all cores | builds/h |
| `mc_k1x` | Full software build | s |
| `st_k2` | 3D rendering on one core | M samples/s |
| `st_k1` | Compiling code on one core | builds/h |
| `b_k3` | Compressing a file | ms |
| `b_k4` | Opening a photo | ms |
| `b_k5` | Reading structured data (JSON) | ms |
| `b_k6` | Starting a small script | ms |
| `r_1ms` | Reacting from rest: a 1 ms task | % |
| `r_10ms` | Reacting from rest: a 10 ms task | % |
| `c_k4` | Opening a photo from rest | ms |
| `c_k6` | Starting a small script from rest | ms |
| `m_lat1` | Memory delay | ns |
| `m_latn` | Memory delay while every core uses memory | ns |
| `j_idle` | Timer punctuality on an idle computer (worst 1 in 1000) | µs |
| `j_load` | Timer punctuality while other cores are busy | µs |
| `u_k2` | 3D rendering: gain from the newest instructions | × |
| `u_k3` | Compression: gain from the newest instructions | × |
| `u_k4` | Opening a photo: gain from the newest instructions | × |
| `u_k8f` | Matrix maths with decimal numbers: gain | × |
| `u_k8i` | Matrix maths with small whole numbers: gain | × |

A key the app does not know is reported when it starts (on the terminal), so
typos do not pass silently.
