# WebKit benchmark

Measures Surfer on WebKit (`userspace/webkit/browser.c`) before and after a
change, on real web sites and on a few local pages, on a Linux host.

```sh
tests/webkit-bench/run.sh            # HEAD against the working tree
tests/webkit-bench/run.sh v1.10.3    # a release against the working tree
tests/webkit-bench/run.sh HEAD .verify/webkit-bench --reps 1 --only site-cnn,tabs-8-on-4GB
tests/webkit-bench/run.sh HEAD .verify/webkit-bench --proc-before icda-new   # both builds on today's /proc
```

Needs Linux and Docker; the container runs privileged. Results land in
`.verify/webkit-bench/campaign/` (one JSON file per run, `summary.json` with
the medians, screenshots of the first run of each scenario).

## What runs

The browser is the real `browser.c` and `gui.c`, built with Surfer's engine
(`userspace/webkit/engine`: WPE WebKit 2.54.3 for Alpine 3.20, what ICDA
ships; `run.sh` builds it first when it is not) and drawing with Mesa's software
renderer (ICDA's "Software" mode). Only ICDA's window manager is emulated
(`host/icda_sys.h`): it hands out the window, composites what `gui_flush`
publishes as ICDA's compositor does, counts it, and types and scrolls from a
script.

`host/procemu.c` makes the host's `/proc` look like ICDA's, so WebKit sees
memory as it does there: the "before" build runs with ICDA 1.10.3's `/proc`
(`/proc/meminfo` rendered once when opened, no `/proc/self/statm`), the
"after" build with the `/proc` this change gives the kernel. Both see a
machine of `--ram-mb` (8 GB, or 4 GB for the tab scenario) and, with
`--enforce`, are limited to it: past it the kernel ends a process, as ICDA's
does when no page is left.

## What is measured

For all the browser's processes together (cgroup + perf): CPU time, system
calls, page faults and context switches. On ICDA every system call and page
fault takes the kernel's one lock (`kernel/cpu/smp.c`), so these counts are
what the browser costs everything else there. Also: frames WebKit hands over
and visible updates (window flushes), memory (sum of PSS), web processes,
load times, crashes, out-of-memory kills, discarded tabs.

| scenario | what it shows |
|---|---|
| `local-anim-hidden` | an animation under an opaque banner (nothing on screen changes) |
| `local-anim-visible` | a small animation in view |
| `local-static` | nothing moves (control) |
| `local-raf-canvas` | a canvas redrawn every animation frame (motion meant to be seen) |
| `local-video` | a 30 frames-a-second video playing |
| `local-ticker` | a number changed ten times a second (a ticker, a countdown): each change seen, the frames between change nothing |
| `local-mse` | a YouTube-like player: 720p VP9 through Media Source Extensions |
| `local-background-tab` | the box-shadow animation (repainted on the main thread) in a tab not shown |
| `local-background-tab-compositor` | a transform/opacity animation (run by the compositor thread) in a tab not shown |
| `site-*` | load, 15 s idle, 10 s of scrolling on a real site (down for 6 s, back up for 4) |
| `navigate-6-sites` | six sites one after another in one tab: web processes left behind |
| `tabs-8-on-4GB` | eight heavy sites in tabs on a 4 GB machine |

Real sites change between runs (banners, ads, A/B tests); runs of the two
builds alternate, and `analyze.py` reports medians.

## Files

- `run.sh` - builds the image and both browsers, runs the campaign
- `campaign.py` - the scenarios, both builds alternately
- `bench.py` - one run: starts the browser, drives it, measures it
- `analyze.py` - medians per scenario and build (`summary.json`)
- `host/icda_sys.h` - ICDA's window manager, emulated
- `host/procemu.c` - ICDA's `/proc`, emulated (LD_PRELOAD)
- `pages/` - the local pages
- `ppm2png.py` - screenshots to PNG
- `latency.py` - how soon wheel notches reach the screen, from the traces
  `bench.py --env BENCH_TRACE=file` writes (`pages/long.html` is made for it)
- `functional.py` - the browser's functions driven as a person drives them
  (the address bar, a click and typing into a page, Back and Forward, a new
  tab, switching and closing tabs, reloading, a link that opens a new tab
  and whether that page gets Surfer's video script and no WebGL), each step
  checked by the page's title: `python3 functional.py --bin <icda-webkit>` in the container
