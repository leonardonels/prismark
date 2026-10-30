# Desktop app

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

`prismark-gui` is the desktop front-end: launch it, pick a test group at the
top, and press ▶ Run on a test card or "Run all". The Quick switch beside it
chooses a **full run** (the default: accurate and ranked; about 30 minutes for
all tests on a laptop) or a **quick run** (minutes, a first look). Each test is ranked on
its own, with your latest run highlighted against reference systems; the
details panel shows the machine state, capabilities, responsiveness curve and
scaling.

```sh
prismark-gui                                  # or: prismark-gui --run [TEST] [--full], e.g. --run mc_k2
```

Once [installed](building.md#install-adds-prismark-to-the-applications-menu), it is also
started from the applications menu like any other app.

- Built automatically when Qt Widgets and Svg are found (Qt 6, or Qt 5.15;
  the packages are in [Building](building.md#building)); `-DPRISMARK_GUI=OFF` skips it. Its typefaces (Inter, Saira and IBM Plex Mono) are fetched at pinned versions and compiled in.
- **Isolation.** Runs execute in the `prismark` runner next to the app, as a
  separate process. During a run the main window closes and only a small
  measuring window remains, static except when a new phase begins;
  closing it cancels the run (the partial result is kept).
- **Quick runs** skip the warm-up, use short series, compile every 8th unit in
  Compiling code and build once. Their numbers are rough, so they are not
  ranked: a test page shows your latest quick run under its ranking, with a
  link to its report, until a full run replaces it. The stress test has no
  quick run (the switch is hidden on its tab); its length and limits are set on its tab.
- **As configured.** The app never changes power settings: every test measures
  the computer as it is set up. Where the processor's power is readable only
  with administrator rights (Windows; Linux since 5.10), it offers to get them
  before a run, and runs without watts if you decline.
- **Hands off.** Cold start and periodic measure waits and wake-ups. Keyboard
  or mouse input during them skips that test: its series are discarded, the
  result lists it as skipped, and the run continues. On GNOME the app watches
  through Mutter's idle monitor (event-driven, no polling); elsewhere on Linux
  it polls the ScreenSaver idle time every 2 s; on Windows and macOS the
  runner reads the last-input time itself. The method is recorded with the
  result.
- **Compile tests.** Compiling code and Full software build need the
  one-time [setup](building.md#compile-tests-setup). When you run one, the app checks
  each step: missing tools and libraries come first, with the install commands
  for your distribution (`apt` on Ubuntu, `dnf` on Fedora); preparing the
  snapshot is always the last step, started with **Prepare now**, and shows
  its progress.

- Every run is saved in `results/runs` of the shared results folder (see
  [Command line](command-line.md)) and reloaded at start, along with runs from the command line; the
  compile-test snapshot lives in `k1x/` beside `results/`. Files from older
  versions are moved into this layout on first start.
  Menu › Open result imports files; to compare two runs side by side, use
  `prismark compare` on the command line.
- The reference systems in the rankings come from the repository's
  [`references/`](../references/README.md) folder, one file per system, built into
  the app: add a file to add a system, delete it to remove one. They are result
  files from real machines; placeholders (example values typed in by hand) are
  still accepted and labelled as such. Users can add their own in
  `results/references` of the shared results folder, without rebuilding.

`apps/viewer` is the static web page for cross-device comparison:
open `index.html` and load or drop result files.
