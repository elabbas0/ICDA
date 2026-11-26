# Surfer on WebKit: second round

Before: `391fbba` (the first round's pacing: 60 frames a second, 15 after half a second of frames that
changed nothing). After: this change, on top of [PR #3](https://github.com/elabbas0/ICDA/pull/3) (`8538c32`).

Measured as in the first round ([README.md](README.md)): `tests/webkit-bench`, the real `browser.c`
and `gui.c` built against WPE WebKit 2.40.5, Mesa's software renderer, ICDA's window manager
emulated, a 4-core Linux host, real sites loaded live. Not measured on ICDA hardware; the GPU path
was not run.

## What changed

- **Frames at the rate the picture changes.** Each frame is the whole page put together again in
  software, whatever changed: up to 70 ms of processor time on pages with blurred or layered parts.
  A page whose banner changed ten times a second was drawn 62 times a second, 52 of them the same
  picture. Now frames come at 60 a second while a key or the pointer is in use, while the picture
  changes frame after frame (video, scrolling, smooth animation) and while the page loads;
  otherwise at twice the rate of the changes of the last second (15 to 60 a second); 15 a second
  after half a second with no change, 4 after five seconds (one second in the campaign below;
  see Wikipedia there).
- **No AV1.** GStreamer's AV1 decoder here is libaom's, on one processor at half VP9's speed;
  with it YouTube sends AV1 and 1080p stutters. Turned off (`GST_PLUGIN_FEATURE_RANK`), sites
  send VP9.
- **From PR #3:** a script at the start of every page tells video players what plays here (no
  AV1; 720p at 30 frames a second without the GPU, 480p on one or two processors, 1080p at 60
  with the GPU); requests to 88 advertising and tracking networks from other sites' pages are
  blocked (WebKit's content filter); WebGL is off without the GPU; on one or two processors a
  page whose frames take long to draw gets at most two thirds of the time for drawing.
- **Fixed in the PR's part:**
  - WebGL stayed on: making a web view turns WebGL on again in the settings it is given, so the
    setting made before the first tab did nothing (`getContext('webgl')` still answered). It is
    now set on each view after it is made.
  - Pages opened by a link in a new tab (popups, which share their opener's web process) had
    neither the video script nor the content filter: a view made with `related-view` gets its
    opener's settings but not its user content manager. Now given.
  - The script offered AV1 with the GPU; the decoding is on the processors either way.
  - YouTube's own pages are exempt from the filter: when its requests to doubleclick.net and
    googlesyndication.com fail, YouTube is known to stop playing and ask for the blocker to be
    turned off (not seen here: YouTube answers this network with a captcha). The filter has a new
    name, `icda-trackers-3`, so the one compiled before is not used. Checked with a local page:
    google-analytics.com is blocked, an image from wikipedia.org loads, and with the page's own
    host exempt instead of YouTube's, google-analytics.com loads.
- `tests/webkit-bench/functional.py` checks both (a link to a new tab: the page there has the
  script and no WebGL).

## Before and after

`391fbba` against `b48d803` (this change, with the PR), both on WPE WebKit 2.40.5, alternately, 2 runs
each (medians); the same `/proc` for both. Idle: 15 s after load; scroll: 10 s of mouse wheel
(6 s on the test pages). Cores: web, UI and network processes. Updates: frames that changed
something on screen, per second.

| Scenario | Idle cores before | after | Updates/s before | after | Scroll cores before | after | Load before | after |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Animation under a banner (test) | 0.16 | 0.05 (−69%) | 0 | 0 | 0.49 | 0.53 | 0.32 s | 0.28 s |
| Small visible animation (test) | 0.52 | 0.55 | 56.8 | 57.1 | 0.54 | 0.54 | 0.29 s | 0.28 s |
| Canvas redrawn each frame (test) | 0.66 | 0.59 (−10%) | 62.0 | 62.0 | 0.64 | 0.63 | 0.32 s | 0.28 s |
| Ticker, 10 changes a second (test) | 0.12 | 0.11 | 10.0 | 10.0 | 0.12 | 0.12 | 0.30 s | 0.28 s |
| Video, 30 frames/s (test) | 0.47 | 0.48 | 30.3 | 30.3 | 0.46 | 0.46 | 0.41 s | 0.40 s |
| Streaming video, 720p VP9 (test) | 1.50 | 1.58 | 31.3 | 31.2 | 1.58 | 1.59 | 0.32 s | 0.31 s |
| Static page (test) | 0.01 | 0.01 | 0 | 0 | 0.03 | 0.02 | 0.30 s | 0.23 s |
| Wikipedia article | 0.38 | 0.31 (−20%) | 23.5 | 18.4 | 0.65 | 0.68 | 4.3 s | 4.3 s |
| BBC News | 0.09 | 0.07 | 0 | 0 | 0.73 | 0.66 | 4.8 s | 4.2 s |
| CNN | 2.63 | 1.90 (−28%) | 14.2 | 13.6 | 2.57 | 2.89 | 7.7 s | 8.9 s |
| NBC News | 1.46 | 1.26 (−14%) | 56.4 | 54.2 | 2.05 | 2.09 | 18.7 s | 13.8 s |
| ESPN | 0.32 | 0.38 | 0.6 | 0.8 | 1.12 | 1.73 | 4.4 s | 2.5 s |
| The Verge | 0.08 | 0.65 | 0 | 6.1 | 0.82 | 1.72 | 8.6 s | 12.3 s |
| **All** | **8.39** | **7.95** | | | **11.80** | **13.66** | **50.8 s** | **47.9 s** |

Pages that animate where it cannot be seen, or change less often than the frames come, take less
(the hidden animation 69%, CNN 28%, Wikipedia and BBC about 20%); video, smooth animation and
scrolling keep their full rate. ESPN and The Verge take more: the after build blocks their ad
networks, and both then work harder (The Verge's "ad recovery" script, above; ESPN while scrolling,
seen in the filter comparison too: 1.61 against 1.07 cores).

Wikipedia: its banner animates for 1.3 s every 3 s, and with the slowest pace (4 frames a second)
reached after one second without change, the first quarter second of each burst was skipped (23.5
updates a second down to 18.4). The slowest pace now comes after five seconds: 22.3 updates a
second, at the same cost (0.33 cores against 0.35); a hidden animation still settles at 4 frames a
second (0.06 cores either way).

## Video

What decoding costs here (GStreamer alone, `decodebin ! fakesink`; processor seconds for 10 s of
video):

| | H.264 | VP9 | AV1 (libaom) |
|---|---:|---:|---:|
| 720p, 30 frames/s | 1.8 | 2.1–2.35 | 3.7 |
| 1080p | 4.6 (60 frames/s) | 3.4 | 6.8 |

libaom decodes AV1 on one processor, so 1080p at 30 frames a second takes 68% of a core and at 60
more than one. Without it YouTube sends VP9.

Streaming video in Surfer (an MSE player like YouTube's, unmuted, 20 s), web process cores:
720p30 about 1.3, 1080p30 1.6–1.8, 1080p60 2.5–2.7, all frames shown and none dropped. About 55% of
that is Mesa putting the page together in software for each video frame; `LP_NUM_THREADS` changes
nothing, and `LP_NATIVE_VECTOR_WIDTH=128` makes it slower. Hence the PR's 720p30 limit without the
GPU.

## Two processors

The browser and its web processes limited to two processors (`taskset -c 0,1`; Surfer then sees
two and takes the 480p video limit and the busy-page pace), each page 16 s with a scroll from the
6th second, the PR's busy pace on and off (`SURFER_FULL_RATE=1`), 2 runs each. Late timer: how
much later than asked a 50 ms `setInterval` in the page ran (its scripts waiting behind the
drawing).

| Page | Web process cores, busy pace | full rate | Frames/s busy | full | Late timer (avg) busy | full |
|---|---:|---:|---:|---:|---:|---:|
| microsoft.com | 0.95 / 1.02 | 1.11 / 1.12 | 18.7 | 21.8 | 7 / 20 ms | 7 / 10 ms |
| CNN | 1.23 / 1.34 | 1.64 / 1.70 | 18.7 | 28.1 | 14 / 20 ms | 21 / 21 ms |
| Canvas redrawn each frame (test) | 0.45 / 0.55 | 0.46 / 0.54 | 59.3 | 59.3 | 1 ms | 1 ms |
| Small visible animation (test) | 0.41 / 0.47 | 0.42 / 0.51 | 59.3 | 59.3 | 1 ms | 1 ms |

On heavy pages the busy pace takes 12-23% less processor time for fewer frames; scripts wait
about as long either way. Light pages draw in well under a frame and keep 60.

## Advertising and tracking networks blocked

Ten sites with many ads, the filter on and off (`SURFER_TRACKERS=allow`), alternately, 2 runs each
(medians). Idle: 12 s after load finished; scroll: 6 s of mouse wheel. CPU of the web, UI and
network processes.

| Site | Load on | off | Load CPU on | off | Idle on | off | Scroll on | off | Memory on | off |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| weather.com | 1.5 s | 20.5 s | 1.3 s | 49.1 s | 0.91 | 0.50 | 2.68 | 2.02 | 797 MB | 1050 MB |
| NBC News | 14.4 s | 18.9 s | 31.3 s | 42.9 s | 1.45 | 1.45 | 2.32 | 2.33 | 996 MB | 1123 MB |
| The Verge | 5.9 s | 8.1 s | 10.9 s | 15.5 s | 0.94 | 0.11 | 1.01 | 0.94 | 835 MB | 818 MB |
| ESPN | 3.5 s | 4.4 s | 5.9 s | 8.1 s | 0.26 | 0.18 | 1.61 | 1.07 | 637 MB | 751 MB |
| BBC News | 4.3 s | 3.6 s | 6.4 s | 6.1 s | 0.08 | 0.21 | 0.82 | 1.66 | 444 MB | 570 MB |
| CNN | 9.8 s | 9.4 s | 23.6 s | 23.5 s | 2.02 | 2.07 | 2.91 | 2.95 | 1340 MB | 1392 MB |
| Yahoo | 1.6 s | 2.2 s | 1.2 s | 2.7 s | 2.79 | 2.68 | 2.57 | 2.51 | 907 MB | 1047 MB |
| IMDb | 1.0 s | 1.2 s | 0.2 s | 0.4 s | 0.56 | 0.61 | 1.96 | 2.02 | 524 MB | 530 MB |
| Reddit | 0.3 s | 0.6 s | 0.0 s | 0.4 s | 0.44 | 0.43 | 2.02 | 2.20 | 699 MB | 738 MB |
| Dailymotion | 6.1 s | 5.6 s | 5.6 s | 5.8 s | 0.01 | 0.05 | 0.75 | 0.73 | 460 MB | 454 MB |
| **All ten** | **48 s** | **74 s** | **86 s** | **155 s** | **9.5** | **8.3** | **18.7** | **18.4** | **7.6 GB** | **8.5 GB** |

Loading takes a third less time and 44% less processor time, and pages 10% less memory. Idle is
higher on two sites, both times: The Verge fetches `ad.doubleclick.net/favicon.ico?ad=300x250` to
see whether ads are blocked and, when it fails, loads an "ad recovery" script from changing
domains (ivoryandcrow.com, worldrealize.com) that works for a while. Over a minute it averages
0.30 cores with the filter and 0.06 without, but the last 30 s are the same (0.044 and 0.042), and
without the filter the page needs 21 s instead of 6 to finish loading. weather.com finishes
loading in 1.5 s instead of 20 with the filter, so its idle period probably begins while it is
still filling the page in (not checked).


## Tried and not kept

| Change | Why not |
|---|---|
| `WEBKIT_GST_ALLOW_PLAYBACK_OF_INVISIBLE_VIDEOS=1` (for the muted-video problem below) | CNN never finished loading (2 runs of 2) |
| `WEBKIT_GST_DISABLE_GL_SINK=1` (pixels by GStreamer's SIMD code, not a Mesa shader) | progressive `<video>` showed no frame; the local video test went from 30 to 15 frames a second |
| mimalloc for the web process | Speedometer 3.1 4.15, against 4.47 with musl's allocator |
| `LP_NUM_THREADS`, `LP_NATIVE_VECTOR_WIDTH=128` (Mesa) | no change, and slower |

## Known

- About half of the muted streaming videos (MSE) out of view when they load never start
  (unmuted: 8 of 8 start). WebKit 2.40 pauses muted videos out of view, and that pause reaches
  GStreamer before the player has its source; the pipeline then never gets to PAUSED. Not
  fixable from `browser.c`.
- JavaScript is WebKit 2.40's: Speedometer 3.1 scores 4.47 (±0.30) here, headless Chromium 16.3
  on the same host. The main thread is the limit, not drawing (4.22 with pacing off). WebKit
  2.40.5 is from 2023, and three years of security fixes are missing from it. Alpine 3.20 and
  3.21 have nothing newer and later Alpine no WPE WebKit at all, so `userspace/webkit/engine`
  builds the current release (2.54.3) for ICDA; Surfer moves to it once it has passed these
  tests.
