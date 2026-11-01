# The Prismark website

A static site on GitHub Pages: https://leonardonels.github.io/prismark/

Plain HTML, CSS and JavaScript, with no framework and no build tool. Everything in this folder is published, this file
included, so nothing private belongs here.

## Pages and files

| File | What it is |
| --- | --- |
| `index.html`, `main.js` | The homepage; `main.js` draws the stress curve and the barcode |
| `compare.html`, `compare.js` | The ranking: one test at a time |
| `versus.html`, `versus.js` | Head to head: two processors, every test, no total |
| `compare-data.js` | Shared by both compare pages: the tests, how each value is read, how two values compare |
| `data.js` | Fills every `data-fill` element from `data.json` |
| `styles.css` | All the styles; the compare pages' are at the end |
| `data.json`, `references.json` | Generated, not in git (see below) |

## Try it locally

From the repo's root folder:

```bash
python3 tools/site_data.py site/data.json
```

```bash
python3 -m http.server 8765 --directory site
```

Then open http://localhost:8765. Use the server: opening the files directly usually blocks `fetch`, and the compare
pages then have no data.

## Where the numbers come from

`tools/site_data.py OUT` writes two files. Nothing on the site should be typed by hand if the code knows it.

| Value | Source |
| --- | --- |
| `version` | The newest GitHub release that isn't a draft. Betas count, because `/releases/latest` ignores pre-releases and returns 404 while every release is a beta |
| `tests` (12) | The different kinds of work in `ITEMS`, `apps/cli/main.c` |
| `measurements` (23) | The entries in the table in `apps/gui/metrics.cpp` |
| `references.json` | `references/*.json`, cut down to the fields `compare-data.js` reads |

The 12 and the 23 count different things: `prismark tests` lists 20 entries, some jobs run in several setups, and some
tests give more than one result.

The numbers in the HTML are fallbacks, shown if `data.json` fails to load. `site_data.py` stops with an error rather
than writing an empty value.

## Publishing

`.github/workflows/site.yml` runs `site_data.py` and publishes this folder. It runs on a push to `main` that changes
the site, the script, `references/*.json`, `apps/cli/main.c` or `apps/gui/metrics.cpp`, when a release is published,
and by hand from the Actions tab.

**Publishing a release takes a click.** `ci.yml` creates each release as a draft, and drafts are invisible to visitors
and to the site. Check its files, keep "Set as a pre-release" ticked, and press Publish release.

**Releases need a tag rule.** Pages only accepts `main` by default, so a release run is refused without this:
Settings › Environments › github-pages › Deployment branches and tags › add a rule, type Tag, pattern `v*`.

GitHub Pages lets browsers keep files for 10 minutes. If a change doesn't show right after publishing, press
Cmd+Shift+R.

## Rules

- **No overall score.** Each test stands alone. No page adds tests up or counts wins.
- **No K IDs** (K1, K2…) in anything a visitor reads. Plain test names only.
- **No invented data.** Example values are labelled as examples, like the homepage's stress curve.
- **A visitor's result stays in their browser.** It is read locally and only its summary is kept, in `localStorage`,
  until they press Remove.
- **Text from a result file is never inserted as HTML.** Build elements with `el()` in `compare-data.js`.
- **Relative links only,** because the site lives under `/prismark/`.

## The look

Follows the Prismark identity: flat panels on an ink ground with 8 px seams, and no radii, shadows or gradients.

| Token | Value | Use |
| --- | --- | --- |
| `--ground`, `--ink` | `#0C1430` | Background and text |
| `--panel` | `#F7F8F9` | Panels |
| `--paper` | `#E8ECEF` | Alternate panels |
| `--muted` | `#3B4660` | Secondary text |
| `--volt` | `#D6F53B` | Action, and "yours" |
| `--signal` | `#2D5BFF` | Structure and references |
| `--fault` | `#FF4F3A` | Wrong results only |

Type: Saira (condensed for scores, wide for headings), Archivo for body text, and IBM Plex Mono in capitals for labels.

The sketches live on the [design canvas](https://claude.ai/artifact/WPRKkXN3xUdK59DLE8Xzd9), which is private to its
owner until shared.

## How the compare values are worked out

`compare-data.js` follows `apps/gui/metrics.cpp` and `prismark compare`:

- Two results are the same when their 95 % ranges overlap.
- Percentages compare by their difference in points, everything else by ratio.
- "May be noise" means one side has no range.
- Bars show speed, so a longer bar is always faster, whichever way the unit counts.
- Quick runs and results with wrong answers aren't ranked.

**When a test changes in the app, change it here too.** `site_data.py` stops the build if the two lists of tests
differ, but it doesn't compare the calculations.

To check them, compare the head-to-head page against the command line:

```bash
build/macos-clang/prismark compare intel-i9-14900k apple-m2-macos
```

## Known gaps

- **Stress test:** no reference has run it, so that tab is empty until a visitor opens their own result.
- **Vector maths:** only Apple M2 has run it. The other references come from older versions; re-running them fills it in.
- **No Desktop/Laptop filter:** result files don't record it. The power filter says "Not on battery" because some
  machines don't report their power source.
- **Power modes** appear as recorded, for example the Italian "Bilanciato".
- **Download links** go to the Releases page, not straight to each file.

## Still to do

- Dark mode
- Hosting the fonts here instead of loading them from Google
- Stopping `ci.yml` from running the full build on site-only pushes
