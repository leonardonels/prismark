# Command line

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

`prismark` runs, lists, shows and compares results. Installed from a package it is on the `PATH`; in a build from source it is `build/linux-clang/prismark`.

## Runs

```sh
prismark                                      # menu: full or quick run, choose tests, past runs, compare
prismark run                                  # full run of all modes (about 30 min on a 4-core laptop)
prismark run --quick                          # smoke run, a few minutes; not for comparison
prismark --mode short-task,from-rest --tests photo,script,wakeup
prismark run --mode stress --stress all-cores --stress-time 30m  # or until-stopped
prismark tests                                # every test and mode, with the names and IDs it accepts
prismark help                                 # options; `help advanced` for tuning and desktop-app options
```

`prismark` alone never starts a run: on a terminal it opens a menu (`b` at
any prompt goes one step back), and otherwise it exits with a hint. Options without a command (`prismark --quick
…`) still start a run, as before. Runs from the menu and from `prismark run`
include the compile tests when the compile-test snapshot is prepared (in the
app or with `tools/k1x/prepare.py`); `--k1-data DIR` points to another one.

`--tests` and `--mode` take the app's names (`"Opening a photo"`, `photo`), a
short name, or the ID (`K4`, `st_burst`); `--kernels` is the same as `--tests`.
A run refuses tests and modes that do not go together (`--tests photo --mode
all-cores` would measure nothing; `prismark tests` shows which do), and names
any chosen test that runs in none of the chosen modes. Number options are
checked too.
On a terminal the run shows one progress line (overall step, elapsed time,
an estimate of the time left, temperature, what runs now) and keeps only part
headers, notices and warnings on screen; piped, it prints one plain line per
event. The line is redrawn only when the core reports progress, between
measurements, so the display never wakes the machine during a test. Runs
started from the menu print the equivalent `prismark run` command first. Colour is used on a
terminal only, and `NO_COLOR` or `--no-color` turns it off. Ctrl+C stops the
run after the current measurement and saves what was measured; a second
Ctrl+C quits at once.

The exit status says how a run ended: 0 done, 1 failed, 2 wrong usage, 3
the machine was too busy to measure (another program was working; nothing was
measured), 4 the stress test found wrong results, 130 stopped with Ctrl+C (what was measured is saved).

A stress test run with `--stress-time until-stopped` ends with Ctrl+C: that is
its normal end, so the run is saved as complete and the command exits 0.

## Results

Each run writes `prismark-<run_id>.json` with every raw sample, the machine
state at start and end, and the statistics computed from them. It goes in the
results folder shared with the desktop app, so command-line runs appear there
too (`-o FILE` writes elsewhere; runs as root write to the current folder):

```
~/.local/share/prismark/results/        Linux; macOS: ~/Library/Application Support/prismark/results,
├── runs/                               Windows: %LOCALAPPDATA%\prismark\results
└── references/                         your own reference systems (see references/README.md)
```

```sh
prismark list                                 # past runs in the results folder, newest first (local times)
prismark show latest                          # a run's summary again
prismark compare latest intel-i5-1035g1       # the headline results against a reference system
prismark compare 6f07bd9e latest              # ... or against another run
prismark compare 6f07bd9e latest --detail     # every series with 95 % CIs, and the profiles
prismark compare a.json b.json --profiles my-profiles.json
prismark references                           # the reference systems compare accepts
prismark references add latest my-desktop     # save a run as your own reference system (remove NAME)
prismark profiles                             # the default profiles (Daily, Dev, Render, Realtime)
prismark checksums -o checksums.json          # kernel output checksums, no timing
eval "$(prismark completion bash)"            # tab completion (also zsh): commands, tests, runs, references
```

A run is named by its file, its run ID or the first characters of it (as
`list` shows them), or `latest`. Results store their summary, so `show` can
print it again; older result files have none, and `show` says so.

## Comparing

`compare` shows the headline results the desktop app shows, side by side, and
says for each whether A is better, worse or the same within the 95 % ranges:
times and speeds as a ratio (`1.37x better`), percentages as a difference in
points (`38.2 pts worse`), and "may be noise" where one side has no range.
Each side is labelled with its run ID and power source, so two runs of the same
processor can be told apart.
Either side may be a reference system from [`references/`](../references/README.md)
(installed with the program) or from your own `results/references`, named by
its file name; placeholders are labelled as such. `references add RUN NAME`
(or Past runs in the menu) saves one of your runs there, and the desktop app
shows it in its rankings. `--detail` compares every
measured series instead, and needs two measured results. It refuses runs
measured under different capabilities, and reports a
profile only when every kernel it names exists in both runs.

## Machine state and power

Prismark measures the machine as it is configured and changes nothing on it:
no power settings, no frequency limits, no idle states. It needs no root
rights. What it finds (governor, power mode, idle states, temperatures) is
recorded with the result.

On Linux, the power of an Intel or AMD processor is read from its RAPL energy
counter, which only root may read since Linux 5.10. Without it no power is
recorded, and the summary says `power none rapl-needs-root`. To measure power,
run `prismark` with `sudo`, or let every user read the counter until the next
restart (this also exposes it to other programs, which it is restricted to
keep from):

```sh
sudo chmod o+r /sys/class/powercap/intel-rapl:0/energy_uj
```

The desktop app offers this before a run (Allow power readings): it asks for
your password through the desktop's own prompt (`pkexec`) and runs that one
command, nothing else; a reboot restores the restriction. The menu can do it
later too, if you chose "Do not ask again".

A run started with `sudo` (or `pkexec`), from the command line or the desktop
app, still saves to the results folder of the user who started it, and the
files and folders it creates there belong to that user.
