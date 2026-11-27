# Surfer on WPE WebKit 2.54.3

Before: the same Surfer (`browser.c` as of this change) on WPE WebKit 2.40.5, the engine Alpine 3.20
packages and ICDA shipped (2023). After: on WPE WebKit 2.54.3 with libwpe 1.16.3 and WPEBackend-fdo
1.16.1, built for Alpine 3.20 by `userspace/webkit/engine/build.sh`, with Skia painting on the
processor (`WEBKIT_SKIA_ENABLE_CPU_RENDERING`, below).

Measured with `tests/webkit-bench` as in the earlier rounds ([README.md](README.md),
[round-2.md](round-2.md)): Mesa's software renderer, ICDA's window manager emulated, a 4-core Linux
host, real sites loaded live; the two engines in two otherwise identical containers, alternately.
Not measured on ICDA itself (see the end).

## Summary

| | WebKit 2.40.5 | WebKit 2.54.3 |
|---|---:|---:|
| Speedometer 3.1 (3 runs each, alternately) | 5.79–5.87 | 11.2–11.6 |
| Processor use of 13 scenarios at rest, together | 7.02 cores | 5.29 cores (−25%) |
| … while scrolling | 12.35 cores | 8.60 cores (−30%) |
| Time to load them | 65.0 s | 37.3 s (−43%) |
| Memory of the 13 pages | 6.2 GB | 6.9 GB (+11%) |
| Muted streaming videos that start (8 tries) | 6 of 8 | 8 of 8 |
| Eight heavy tabs on 4 GB: crashes, processes ended | 0, 0 | 0, 0 |
| … processor use with all eight open, at rest | 2.84 cores | 1.74 cores |
| … most memory | 3.27 GB | 3.18 GB |
| … the first tab, discarded, shown again after | 4.1 s | 2.9 s |

## Scenarios

2 runs each (medians). At rest: 15 s after load; scrolling: 10 s of mouse wheel (6 s on the test
pages). Cores: the web, UI and network processes. Memory: the browser's processes (PSS).

| Scenario | At rest 2.40 | 2.54 | Scrolling 2.40 | 2.54 | Load 2.40 | 2.54 | MB 2.40 | 2.54 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Animation under a banner (test) | 0.06 | 0.06 | 0.59 | 0.40 (−33%) | 0.4 s | 0.3 s | 224 | 244 |
| Small visible animation (test) | 0.53 | 0.46 (−14%) | 0.60 | 0.45 (−24%) | 0.3 s | 0.2 s | 225 | 244 |
| Canvas redrawn each frame (test) | 0.55 | 0.55 | 0.54 | 0.53 | 0.3 s | 0.2 s | 232 | 263 |
| Ticker, 10 changes a second (test) | 0.11 | 0.09 (−21%) | 0.12 | 0.10 (−22%) | 0.2 s | 0.2 s | 229 | 248 |
| Video, 30 frames/s (test) | 0.42 | 0.38 | 0.39 | 0.41 | 0.4 s | 0.3 s | 263 | 289 |
| Streaming video, 720p VP9 (test) | 0.92 | 0.95 | 0.90 | 1.02 (+13%) | 0.3 s | 0.2 s | 280 | 325 |
| Static page (test) | 0.00 | 0.00 | 0.02 | 0.01 | 0.3 s | 0.2 s | 223 | 243 |
| Wikipedia article | 0.37 | 0.19 (−50%) | 0.68 | 0.61 (−10%) | 4.3 s | 3.7 s | 336 | 371 |
| BBC News | 0.07 | 0.11 | 0.65 | 0.49 (−25%) | 4.4 s | 1.7 s | 438 | 493 |
| CNN | 2.16 | 0.93 (−57%) | 2.92 | 1.48 (−49%) | 9.2 s | 9.2 s | 1343 | 1618 |
| NBC News | 1.20 | 0.87 (−27%) | 1.82 | 1.50 (−18%) | 14.0 s | 12.1 s | 980 | 1015 |
| ESPN | 0.35 | 0.17 (−53%) | 1.71 | 0.94 (−45%) | 3.7 s | 5.4 s | 656 | 670 |
| The Verge | 0.27 | 0.55 | 1.40 | 0.67 (−52%) | 27.1 s | 3.5 s | 785 | 887 |
| **All** | **7.02** | **5.29** | **12.35** | **8.60** | **65.0 s** | **37.3 s** | **6215** | **6909** |

Frames that changed something on screen were the same in both but for Wikipedia, whose animated
banner came in one of the two runs on 2.54 only: with it, 23.6 updates a second at 0.29 cores on
2.54 against 22.2 at 0.37 on 2.40. The Verge at rest is its "ad recovery" script after its ad
networks are blocked ([round-2.md](round-2.md)), which on 2.54 runs within the 15 s after a 3.5 s load
instead of during a 27 s one.

The new engine takes more memory per page, 8–20%; under pressure it gives more back (eight tabs on
4 GB above).

## Video

| 20 s of streaming video, unmuted, alone on the host | Web process cores, 2.40 | 2.54 |
|---|---:|---:|
| 720p VP9 | 1.03 | 1.10 |
| 720p H.264 | 0.94 | 0.96 |
| 1080p VP9 | 1.24 | 1.22 |

Video costs the same on both: the decoding is GStreamer's in both. All frames played, 29.9 a second,
none dropped. 2.54 also reports the video's size (2.40 said 0×0), and muted streaming videos start
every time: 2.40 could pause a muted one as out of view while it loaded, before its player had its
source, and then it never started (2 of 8 tries); on 2.54 8 of 8 VP9 and 8 of 8 H.264 started. Not
shown by 2.54: VP9 in 12-bit 4:4:4 (profile 3, rare on the web), which 2.40 converted; the bench's
test video is now 8-bit 4:2:0. A looping `<video>` needs its server to answer Range requests on 2.54
(web servers do), so the bench serves its pages with `rangeserve.py`.

## Painting without a GPU

WebKit 2.54 paints with Skia, by default through GL, which without a GPU is Mesa's llvmpipe doing a
GPU's work; `WEBKIT_SKIA_ENABLE_CPU_RENDERING=1` has Skia paint with its own code for the processor,
on half of them. Six pages, twice each, alternately: processor use the same (CNN at rest 0.90 cores
against 0.98, the others within 0.05; Speedometer 11.6 against 11.3), frames the same, memory 2–7%
less on every page. Surfer sets it when it draws without the GPU.

## ICDA's kernel

Every system call of Surfer and its WebKit processes, traced on the bench (`strace -f`) on four
pages, a link opening a new tab and typing in a field: 93 calls on 2.54.3, all handled by ICDA's
Linux layer but `sched_setattr`, with which WebKit 2.54 sets each thread's nice level (two calls a
thread). ICDA now accepts it, as it does `sched_setscheduler` and `setpriority`. Against 2.40.5 the
new engine adds `nanosleep`, `timerfd_create`, `timerfd_settime`, `utimensat` and
`prctl(PR_SET_VMA)`, all handled; its `madvise`, `clone` and `mmap` flags are ones 2.40.5 used.

## Memory

Per process, a static page: the web process 157 MB on 2.40, 171 MB on 2.54, nearly all of it the
larger library (file-backed; 2.54's libWPEWebKit is 120 MB); the network process 22 and 28 MB. CNN:
the web process's anonymous memory 1070 MB on 2.40, 1345 MB on 2.54.

CNN alone on an emulated 4 GB machine (Surfer then has WebKit give memory back from 676 MB, harder
from 1024 MB, and end the page at 1638 MB), two runs each unless said:

| | Memory (PSS) | Cores at rest / scrolling |
|---|---:|---:|
| WebKit 2.40.5 (one run) | 1299 MB | 2.42 / – |
| WebKit 2.54.3 (four runs) | 1557–1648 MB | 1.03–1.11 / 1.76–2.02 |
| 2.54.3 with Safari 16.4's user agent | 1565–1580 MB | 0.99–1.04 / 1.59–1.73 |
| 2.54.3, one Skia painting thread | 1514–1596 MB | 1.01–1.08 / 1.77–1.94 |
| 2.54.3, JavaScript heap growing slower¹ | 1523–1559 MB | 0.98–1.01 / 1.66–1.69 |

¹ `JSC_smallHeapGrowthFactor=1.5 JSC_mediumHeapGrowthFactor=1.25`.

About 60 MB of the difference is CNN sending more to a newer Safari; the rest is the engine, and
mostly JavaScript's heap: growing it more slowly saves about 100 MB, but Speedometer 3.1 drops to
7.94 (11.2–11.6 by default), so it is not used. One painting thread saves less than the runs vary.
Nothing changed: the page stays below the 1638 MB WebKit counts against (its anonymous memory, 1345
MB), and with eight heavy tabs on 4 GB 2.54 used less memory than 2.40 (above).

## AV1 through dav1d (measured, not shipped)

GStreamer's dav1d decoder is in GStreamer's Rust plugins (gst-plugins-rs), which Alpine 3.20 does not
package (3.21 does, for the same GStreamer 1.24); the dav1d library itself is in 3.20. With 3.21's
plugin (0.13.3) in the bench container, 10 s of video decoded alone, processor seconds:

| | VP9 | AV1, libaom | AV1, dav1d |
|---|---:|---:|---:|
| 720p | 2.13 | 3.39 | 2.44 |
| 1080p | 3.20 | 5.54 | 3.17 (0.84 s on the clock: all processors) |

In Surfer (`SURFER_MEDIA_ANY=1`, and a GStreamer registry that lists the plugin), streaming AV1 played
every frame at the cost of VP9: 720p 1.32 cores against 1.36, 1080p 1.53 against 1.57. AV1 needs about
a third less data than VP9 for the same picture. Shipping it needs the plugin built for Alpine 3.20
(from the gst-plugin-dav1d crate) or taken from Alpine 3.21's package; neither is done yet.

## The GPU path

Not run: this host has no GPU, no DRM device and no udmabuf, and Surfer's GPU mode needs ICDA's Intel
driver (`/dev/dri/renderD128`) and a GPU renderer (it refuses llvmpipe). Every WebKit and WPE function
it calls exists in 2.54.3. New on 2.54 with the GPU: WebGL is drawn in a GPU process
(`WPEGPUProcess`, in the root), which software mode never starts (WebGL is off there), so the system
call audit above has not seen it.

## Building it

Clang 17, Alpine 3.20's default, cannot compile WebKit 2.54 with GCC 13's C++ library (a parameter
pack bug in std::tuple's C++23 constructors, fixed in clang 18); clang 18 can. ThinLTO needs lld 18,
which Alpine 3.20 does not have, so the engine is built without it. On musl, WPEBackend-fdo needs
`<unistd.h>` and the engine `-lintl`. About an hour and a half on four processors.

## Not yet done

- Surfer with this engine on ICDA itself: here QEMU has no KVM, and ICDA's desktop with WebKit under
  emulation is too slow to test. The system calls above are the part of ICDA it relies on that
  changed; a run on hardware (or QEMU with WHPX/KVM) is the check that remains.
- AV1: measured above; the dav1d plugin is not in the root yet, so AV1 stays off and YouTube sends
  VP9.
- The GPU path, on ICDA hardware with its Intel driver.
