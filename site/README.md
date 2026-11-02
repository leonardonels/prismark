# The Prismark website

A static site on GitHub Pages: https://prismark.leonardonels.com

Plain HTML, CSS and JavaScript, with no framework and no build tool. Everything in this folder is published, this file
included, so nothing private belongs here.

## Pages and files

| File | What it is |
| --- | --- |
| `index.html`, `main.js` | The homepage; `main.js` draws the stress curve and the barcode |
| `motion.js` | The homepage's animations: starts them when seen, and measures the phone wallet |
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

**The domain** is set in Settings › Pages, with a CNAME record in Cloudflare from `prismark` to `leonardonels.github.io`,
proxy off (grey cloud). No `CNAME` file is needed, because the site publishes through Actions. The old address,
`leonardonels.github.io/prismark/`, redirects here. If Cloudflare's proxy is ever turned on, set its SSL mode to Full
(strict): "Flexible" can cause a redirect loop.

GitHub Pages lets browsers keep files for 10 minutes. If a change doesn't show right after publishing, press
Cmd+Shift+R, or Cmd+Option+R in Safari, where Cmd+Shift+R opens Reader instead.

## Rules

- **No overall score.** Each test stands alone. No page adds tests up or counts wins.
- **No K IDs** (K1, K2…) in anything a visitor reads. Plain test names only.
- **No invented data.** Example values are labelled as examples, like the homepage's stress curve.
- **A visitor's result stays in their browser.** It is read locally and only its summary is kept, in `localStorage`,
  until they press Remove.
- **Text from a result file is never inserted as HTML.** Build elements with `el()` in `compare-data.js`.
- **Relative links only,** so the site works at any address, including a local server.

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

### Dark mode

Royal blue, at the end of `styles.css`. It follows the visitor's system setting; there is no switch on the page. Volt and
Signal panels don't change, only the neutral surfaces do:

| Token | Light | Dark |
| --- | --- | --- |
| `--ground` | `#0C1430` | `#0A1238` |
| `--panel` | `#F7F8F9` | `#16246B` |
| `--paper` | `#E8ECEF` | `#1E3288` |
| `--muted` | `#3B4660` | `#C3CDF0` |
| `--text` | (uses `--ink`) | `#F2F4F8` |
| `--line` | (`#C9D0DA`, written directly in the rules) | `#5468C0` |
| `--bar` | (uses `--signal`) | `#9DB2FF` |

- `--ink` stays dark in both modes, because it's the text on Volt, Signal and light surfaces. Text on dark surfaces uses
  the body colour, `--text`.
- Data bars use `--bar`, because Signal is too faint on the dark panels. "Yours" is an ink bar in light mode and a Volt
  bar in dark mode, except inside its own Volt row.
- The PRISMARK tab, the download panel and the stress-test text panel stay white in both modes. The footer is white in
  dark mode only.
- The Stress test card and the command-line panel keep light mode's `#141D3B` in both modes, with no outline.
- Tabs, test cards 01 to 04, the Method panel, the ranking and the head-to-head groups share `--paper` (`#1E3288`) in dark
  mode.
- On the compare pages in dark mode, the filters and reference passes are white, the test list and selection bar use
  `#141D3B`, and the current test's title sits on a Signal strip with a ghost group number. Light mode hides that number.
- Main buttons are ink in light mode and Volt in dark mode. The current tab or test flips to a light surface.

To test it, set your system to dark, or use the Rendering panel in the browser's DevTools.

The sketches live on the [design canvas](https://claude.ai/artifact/WPRKkXN3xUdK59DLE8Xzd9), which is private to its
owner until shared.

### Motion

Only three things move, all on the homepage. Each plays once, and with reduced motion turned on, none of them do.

- **Stress curve:** every bar starts at peak speed and sinks to its value, left to right, and the last six fade to Volt as they sink. It
  plays when 40 % of the chart is on screen. It is still labelled as an example.
- **Numbers:** each `.score` rolls through digits and lands on its value. It rests on the real value before rolling, so a
  wrong number never sits on the page. The version also rolls again each time a download link is clicked. `motion.js` builds the reels after `data.js` has filled the value, because filling
  a value replaces what's inside the element.
- **Phone wallet** (640 px wide or less, and at least 650 px tall): panels stick and close up, each keeping a 26 to 30 px strip
  with its label, so the page ends with every section still readable at the top. `--k` in `styles.css` sets each panel's
  place. The Stress test card is hidden here, because the stress section is the ticket with that label.
  `.deck` rows become `display: contents`, so all the panels share one parent, and sticky only works inside its parent.
  That's why the links land on empty `.anchor` elements: a `display: contents` row has no box to scroll to, and a stuck
  panel isn't where a link back to it expects it to be. A panel taller than its space pins by its bottom instead, using
  the `--h` that `motion.js` measures, so nothing gets covered before it can be read.
  Tapping a closed ticket's strip scrolls back and reopens it; a closed ticket's own link doesn't fire.

Safari's top bar stays yellow at the top of the phone wallet, despite the `theme-color` in `index.html`, probably because it takes its colour from the stuck Volt panel. Accepted for now.

The page's scrollbar is hidden in `styles.css`, because of a flashing scrollbar in Safari that also appears on GitHub.

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

- A manual light/dark switch, if following the system setting turns out not to be enough
- Hosting the fonts here instead of loading them from Google
