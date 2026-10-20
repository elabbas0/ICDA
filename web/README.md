# ICDA in the browser

`web/` is a static page that boots ICDA inside the browser. The PC is
emulated by [qemu-wasm](https://github.com/ktock/qemu-wasm) (QEMU 8.2 compiled
to WebAssembly, with a JIT from guest code to Wasm). ICDA is a 64-bit kernel,
so 32-bit-only emulators such as v86 cannot run it.

On a recent laptop the page reaches the "Welcome to ICDA" screen about 80
seconds after the download finishes.

## What is in here

| File | What it is |
| --- | --- |
| `index.html`, `icda.js` | The page. It downloads the image, starts QEMU, draws frames to a canvas and sends keyboard and pointer events back. |
| `coi-serviceworker.min.js` | Adds the COOP/COEP headers that SharedArrayBuffer needs, for hosts like GitHub Pages that cannot set headers. MIT, from [coi-serviceworker](https://github.com/gzuidhof/coi-serviceworker). |
| `qemu-wasm-canvas.patch` | Changes to qemu-wasm: a `-display canvas` backend (`ui/canvas.c`), and a fix for glib comparators that trap in WebAssembly. |
| `build-qemu.sh` | Builds QEMU with the patch in Docker and writes `web/qemu/`. |
| `kernel.iso` | Built by `make web-iso`: BIOS-only GRUB with a gzip-compressed kernel, about 22 MB instead of 62 MB. |

`kernel.iso` and `qemu/` are build outputs and are not committed.

## Building and running locally

```sh
make web-iso            # web/kernel.iso
sh web/build-qemu.sh    # web/qemu/ (Docker; slow the first time)
cd web && python3 -m http.server 8000
```

Open <http://localhost:8000/>. The service worker reloads the page once to
turn on cross-origin isolation.

## Emulated machine

`-m 256M -vga std -device qemu-xhci -device usb-tablet -nic none`, with the
default `qemu64` CPU (no AVX). The USB tablet gives ICDA an absolute pointer,
so the guest cursor follows the browser pointer without pointer lock. There is
no network adapter, because the WebAssembly build has no network backend.

## How the canvas display works

QEMU runs in a worker (`PROXY_TO_PTHREAD`), so it cannot touch the DOM. On each
display refresh, `ui/canvas.c` converts the guest framebuffer to RGBA in its
own buffer and asks the page thread to copy it into the canvas; a flag in
shared memory stops it from overwriting a frame the page has not copied yet.
Input goes the other way through a ring of events in shared memory that the
page fills with `Atomics` and QEMU drains on the next refresh.
