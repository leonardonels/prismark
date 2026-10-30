# Design

[README](../README.md) · [Measuring](measuring.md) · [Command line](command-line.md) · [Desktop app](desktop-app.md) · [Building](building.md) · [Packaging](packaging.md) · [Development](development.md) · [Design](design.md)

The visual language of the desktop app, its packages and this repository's pages. The code is the source of truth:
colours and typefaces are in [`apps/gui/theme.cpp`](../apps/gui/theme.cpp), painted components in
[`apps/gui/widgets.cpp`](../apps/gui/widgets.cpp), dialogs in [`apps/gui/stub.cpp`](../apps/gui/stub.cpp).

## Principles

- **Flat blocks.** Square corners, no gradients, no shadows drawn by the app. Panels sit on a background one step
  darker (light) or lighter (dark) than themselves.
- **Ink, paper, volt and signal blue.** Navy ink and off-white paper for everything; volt for actions and your own
  result; signal blue for structure and reference machines. Red only for wrong results and failures.
- **Quiet while measuring.** Nothing animates or repaints on a timer: a window paints only when Qt asks, so it never
  wakes a core during a test.
- **Each test on its own.** There is never an overall score, in the app or in any picture of it.

## Colour

| Token | Dark | Light | Use |
| --- | --- | --- | --- |
| bg | `#070C1F` | `#DDE2E8` | window background |
| card | `#141D3B` | `#F7F8F9` | panels, dialogs |
| raised | `#1F2B52` | `#E8ECEF` | recessed tabs, secondary buttons, code blocks |
| border | `#2A3760` | `#C9D0DA` | hairlines |
| text | `#E8ECEF` | `#0C1430` | text, your bars in light mode |
| muted / faint | `#AEB8D6` / `#8591B5` | `#1E2742` / `#343E5A` | secondary text |
| accent (volt) | `#D6F53B` | `#D6F53B` | primary buttons, the current tab's foot, your result |
| onAccent | `#0C1430` | `#0C1430` | text on volt |
| link / ref | `#D6F53B` / `#5C80FF` | `#2D5BFF` / `#2D5BFF` | links; reference machines' bars |
| good / bad | `#D6F53B` / `#FF6F5E` | `#3B5200` / `#C2261A` | verdicts |

Fixed colours outside the theme: signal blue `#2D5BFF` (the disk image, run dialogs), alert red `#FF4F3A` (error
dialogs), and the dialogs' dashed rule, `#3A4874` dark and `#9AA5B4` light.

## Type

- **Saira** (variable width and weight): headings wide (width 112–125, bold or extra bold) and scores condensed.
- **Inter**: running text and buttons.
- **IBM Plex Mono**: units, IDs, clocks, and small labels in capitals with tracking (`labelFont`), such as
  `TESTS · ALL CORES` or `RUN SETTINGS`.

All three are SIL OFL 1.1, fetched at pinned versions and compiled into the app.

## Logo and icons

One beam enters a prism and leaves as three bars of different lengths: the separate results
([`apps/gui/icons/prismark.svg`](../apps/gui/icons/prismark.svg)). In the app it is ink on a volt tile. The app icon
is a volt prism on an ink tile (`prismark-app.svg`); at 32 px and below, the prism and its middle bar only
(`prismark-app-small.svg`). The macOS icon, the Windows `.ico` and the Linux icon are all drawn from these SVGs.

## Components

- **Segmented tabs.** Square tabs; the current one is a panel with a volt foot, the others recessed. Numbered tabs
  carry `01`, `02` above their name.
- **Toggle.** A square switch that says ON or OFF: OFF recessed with the knob left, ON volt with an ink knob right.
- **Test card.** Name, what it measures, your latest score in condensed Saira, and its Run button; volt when
  selected.
- **Pill.** A small tag in mono capitals on a tinted block: `YOUR RUN`, `REFERENCE`, `PLACEHOLDER`.
- **Ranking.** One row per machine; your bars volt (dark) or ink (light), references blue, a thin line for the 95 %
  range.

## Dialogs

A dialog is a stub torn off the pass: a code, one sentence, two buttons.

- **Strip** with a code in mono capitals and, on the right, a tag: red `ERR / …` with a warning sign for errors,
  blue `RUN / …` for the run (`RUN / CANCEL` · `Part 07 of 12`), volt `SYS / …`, `RES / …` and `APP / …` for the
  system, your results and the app.
- **Heading** in wide Saira, then **one sentence** in muted text. Paths and exit codes go in a mono block, never in
  the sentence.
- Optional: the run's progress as three blocks (done in ink, now in volt, to come outlined), a square checkbox, or
  what the question needs (a name field).
- **Buttons** under a dashed rule, side by side, equal width. The volt one is Enter's. When one choice ends
  something (Cancel run, Move to trash), it is the outlined button and the safe choice is volt.

## Writing

- Name things by what they do: "Opening a photo", not K4. IDs appear only where the command line needs them.
- Plain sentences, no exclamation marks, no "please". Say what happened and what is kept: "The tests finished so
  far are kept as a partial result. It is not ranked."
- Numbers with their unit and range; "may be noise" when there is no range.

## Pictures in this repository

The README's banner and screenshots are in [`docs/images`](images): a dark and a light version of each, shown by
GitHub to match the reader's theme. Screenshots come from the app itself (`PRISMARK_GUI_SCREENSHOT=file.png
prismark-gui`, with `QT_QPA_PLATFORM=offscreen` and the theme set in the app's menu), at 1440 × 900.
