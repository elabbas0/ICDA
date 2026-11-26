# ICDA
ICDA (short for I Can Do Anything) is a modular AI-based operating system. Fully customizable without any code knowledge required

## License

ICDA is source-available under the **PolyForm Noncommercial License 1.0.0**
plus **ICDA Additional Terms** — see [LICENSE](LICENSE).

In short: you may use, modify, and fork ICDA for noncommercial purposes, but
you may **never sell it** or market it (or any derivative) as a different
product, and you must **credit the ICDA project** even in minimally changed
forks. Commercial use requires a separate license.

## Build and test

The easiest portable path is Docker. It provides the compiler, NASM, GRUB ISO
tools, and QEMU in one image.

On Windows, run the all-in-one setup/build command from `cmd.exe` or
PowerShell:

```cmd
scripts\icda.cmd ready
```

Do not run the `.ps1` file directly on Windows. The `.cmd` entry point bypasses
the local script execution policy for this repo command.

That command checks the PC, launches the elevated setup when needed, then uses
an automatic backend order on Windows: WSL2 first, Hyper-V as the fallback if
the WSL path fails during setup. It builds the Docker image, builds
`kernel.iso`, and runs the QEMU smoke test once Docker is available.

Git Bash can run `scripts/icda.sh`, but it cannot replace WSL2 or Hyper-V as
Docker's Linux container backend. Docker Desktop still needs a supported
Windows version plus WSL2 or Hyper-V.

Useful subcommands:

```cmd
scripts\icda.cmd check
scripts\icda.cmd install
scripts\icda.cmd build
scripts\icda.cmd smoke
scripts\icda.cmd qemu
scripts\icda.cmd qemu -Headless
scripts\icda.cmd qemu -Uefi
scripts\icda.cmd ready -UseWsl
scripts\icda.cmd ready -NoWsl
```

On Linux/macOS shells with Docker installed:

```sh
./scripts/icda.sh ready
```

Manual Docker commands are still possible:

```sh
docker build -t icda-toolchain .
docker run --rm -v "$PWD:/workspace" -w /workspace icda-toolchain make
docker run --rm -v "$PWD:/workspace" -w /workspace icda-toolchain make qemu-smoke
```

If the native toolchain is installed locally:

```sh
make
make qemu-smoke
make qemu
```

## Local assistant and input

Codex runs only in Terminal. Type `codex` to open the official interactive
CLI, `codex login --device-auth` to sign in with ChatGPT, or
`codex --version` to check the installation. Its own `/model` command and
`--model` option select an account model. The launcher keeps stdin/stdout
attached to Terminal and enables live web search and full system-file access.
It runs in foreground mode (`--no-daemon`) on ICDA.
For ChatGPT sign-in from a phone or another computer, run
`codex login --device-auth`, open the displayed link there, and enter the
one-time code. Enable device-code login in ChatGPT's Security settings if
required. Browser sign-in (`codex login`) returns to `localhost:1455` on
ICDA, so that flow must finish in a browser on ICDA itself.
Installed-system test results and screenshots are in the
[1.8.25 verification report](docs/verification/codex-1.8.25/README.md).
Fresh sign-in startup and callback tests are in the
[1.8.26 verification report](docs/verification/codex-1.8.26/README.md).
The built-in assistant has no Codex provider or Codex commands.

In Terminal, drag across text and press **Ctrl+Shift+C** to copy it. Paste
into Terminal with **Ctrl+Shift+V**, or into Surfer's address bar with
**Ctrl+V**. Terminal also accepts OSC 52 clipboard writes from CLI programs.
[Clipboard verification and screenshots](docs/verification/clipboard-1.8.27/README.md).

The 1.8.28 TCP send-buffer fix and authentication test limits are documented
in the [network verification report](docs/verification/auth-network-1.8.28/README.md).

Prepare the verified official package with
`powershell -ExecutionPolicy Bypass -File scripts/prepare-codex.ps1`.
The package lives in ICDA's `linux/usr`, each user's credentials in their own
`/home/.codex` (an earlier sign-in moves there once), and temporary SQLite
data in `/tmp/codex-state`.
No Windows model files are used. `/home/AGENTS.md` describes BusyBox and
ICDA storage; this is a separate CLI session with its own history.

For the built-in Qwen assistant, open **Settings > Assistant**, select
**Local**, and click **Model** to choose Qwen 2.5 0.5B (469 MiB), 1.5B
(1066 MiB) or 3B (2007 MiB), or Qwen3 4B (2382 MiB). The bigger two are
much better at using ICDA's tools and want about 6 GB of RAM. Click
**Download from Hugging Face** to download only that model.
Progress appears in the Assistant pane. The selected file is streamed from
Qwen's official pinned HTTPS URL to ICDA's own partition, checked against its
expected byte size and SHA-256 hash, and renamed into place after verification.
Existing model files are preserved on failure. No restart is required.
Interrupted connections reconnect and resume with HTTP ranges. The model still
needs enough free RAM to run; insufficient memory is reported in the assistant.

The assistant knows ICDA (its apps, files, shortcuts and settings, from
`/usr/share/icda/assistant/guide.txt`) and acts on the computer with tools:
it lists, reads and writes files and makes folders (writing only in `/home`,
`/tmp` and `/volumes`, never deleting; a replaced file's old text is kept in
`/tmp/ai/previous`), searches the web, reads web pages, opens apps and sets
reminders. Web search asks DuckDuckGo, and Wikipedia when DuckDuckGo turns
automated requests away; for full results, news included, add a free Brave
Search API key to `/home/.config/ai.conf` as `search brave <key>`. Local and cloud models use the same tools, up to six per question.
The local model uses one thread per processor core and reads the instructions
only once, so follow-up steps start right away.

The assistant also supports `/model install 0.5b` (or `1.5b`, `3b`, `4b`)
and `/model status`. Choose the downloaded model in Assistant settings before
asking it a question. OS updates and offline bundles never fetch or include
Qwen weight files; the updater ignores legacy model metadata rows. Developer
builds can still prepare model assets using `scripts/prepare-ai.ps1`.
Settings > Mouse controls sensitivity, smoothing, and pointer enhancement
(acceleration), applied while the desktop is running. Relative PS/2 and USB
mice use these controls; absolute tablets retain their screen coordinates.
Wi-Fi strength uses one, two, or three arcs. Dual-band SSIDs appear separately
with their frequency band, subject to the adapter and firmware's channel map.

Sound output follows the codec's connected pin-to-DAC routes, preserving
selector inputs and opening the amplifiers along those routes. The volume
preview and sound switches play feedback even with automatic effects off.
Run `make hda-codec-test` in the toolchain container to check codec selection,
disconnected pins, speaker/headphone paths, and mixer input routing.

DHCP keeps one transaction ID through discovery and the address request,
pads BOOTP messages to 300 bytes, and receives replies while application
sockets remain open during reconnection. `make dhcp-test` checks a strict
hotspot, delayed replies, and malformed packets. Wi-Fi errors distinguish
a missing DHCP offer from a missing acknowledgement.

## Surfer on WebKit

Surfer shows a page's frames at the pace the page needs. Without a GPU each
frame is the whole page put together again in software, whatever changed (up
to 70 ms of processor time on a page with blurred or layered parts), so a
frame that shows nothing new is waste. Frames come 60 a second while a key or
the pointer is in use, while the picture keeps changing (a video, a scroll, a
smooth animation) and while a page loads; otherwise as often as the picture
really changes (twice as often: a ticker that changes ten times a second gets
20 frames, not 60); 15 a second after half a second with no change, 4 after
five seconds, and one a second for tabs that are not shown. A key press, the
pointer or a change brings the full rate back at once. On one or two
processors a page whose frames take long to draw gets at most two thirds of
the time for drawing, so its scripts still run (`SURFER_FULL_RATE=1` turns
that off). Only the parts of a frame that changed are copied to the window.

Video is offered only as the machine can play it: a script at the start of
every page tells players such as YouTube's that there is no AV1 and nothing
above 720p at 30 frames a second (480p on one or two processors, 1080p at 60
with the GPU), so they choose H.264 or VP9 at a size that plays;
`SURFER_MEDIA_ANY=1` lifts the limits. libaom's AV1 decoder is not used at
all: it decodes on one processor at half VP9's speed. Without the GPU, pages
are told there is no WebGL (drawn by the processors it takes them all and
still crawls). Requests to 88 advertising and tracking networks from other
sites' pages are not made, except on YouTube's own pages, which stop playing
when they fail (`SURFER_TRACKERS=allow` makes them all). Known in
WebKit 2.40 with GStreamer 1.24: about half of the muted streaming videos
(MSE) that are out of view when they load never start; WebKit pauses muted
videos out of view, and that pause comes before the player has its source.

Memory is watched, because swap is far slower than memory (and a live start
has none). A page gives memory back as it
grows, from a sixth of the RAM (at most 1 GB), and a page that keeps growing
past two fifths (at most 2.4 GB) is stopped with *The page used too much
memory*. Sites you leave do not stay in memory, and below 6 GB of RAM going
Back loads the page again from the disk cache. When the computer runs short
of memory, the tab shown longest ago is discarded: its name stays in the tab
strip, and it loads again when you select it. Tabs playing sound are kept.
`/tmp/surfer.log` shows the limits in use. `tests/webkit-bench/run.sh`
measures Surfer before and after a change; the measurements of these changes
are in [docs/verification/webkit-performance](docs/verification/webkit-performance/README.md)
(frame pacing by change, video, the advertising filter:
[round-2.md](docs/verification/webkit-performance/round-2.md)).

The engine (WebKit with its JavaScript JIT, and GStreamer for video and
sound) is in the Linux root, `/linux`. Without it Surfer falls back to its
classic engine, which cannot run sites such as YouTube. Releases 1.11.1 and
1.11.2 were published from a root without WebKit, and updating to them
removed it; a release built from the full root brings it back. So
`tests/linux/mkroot.sh` always builds the root a release ships, with WebKit,
GStreamer and the Windows layer (`ICDA_ROOT=minimal` makes a small one for
tests of the Linux personality, never to be released);
`scripts/ota-release.sh` refuses a root without WebKit, or with fewer than
half of the last release's Linux files; and the updater keeps a system's
WebKit when a release does not have it.

## Dialogs

A program asks the window manager for a dialog (an error, a warning, or a
question with up to four answers) and the window manager draws it, so every
dialog looks and works the same: Enter presses the default button, Escape the
cancel one, Tab and the arrow keys move between them. A program's dialog sits
on its window, which dims and takes no input until the dialog is answered;
the program keeps running meanwhile. Programs without a window and ICDA
itself put theirs in the middle of the screen: when a program started from
the desktop stops on a fault, or is ended because memory ran out, a dialog
names it and its window goes. (Before, such a program vanished silently, and
running out of memory while a program's stack grew stopped the computer.)

Dialogs are not notifications. A notification (`/dev/notify`, the bell in the
taskbar) reports something and waits in the notification centre; a dialog
needs an answer, and is not kept.

Programs ask with `ic_app_dialog` or `ic_app_alert` (`userspace/ic_app.h`),
which call back with the answer, or without ic_app with `ic_dialog_open`,
`ic_dialog_result` and `ic_dialog_run` (`userspace/ic_dialog.h`). Underneath
is `/dev/dialog`, whose format `kernel/dev/dialog.c` describes. In Terminal,
`dialog [-e|-w|-q] [-b "Yes|No"] <title> [message]` asks and prints the
button chosen.

## Installing ICDA

Started from a USB drive (`kernel-usb.img`, or `kernel.iso` written to a
stick), ICDA first asks whether to **Install ICDA** or **Try ICDA**. Nothing
on the computer changes either way until the installer's last step. Trying
needs no account: the live session runs from memory and keeps nothing, and
Install ICDA stays first on its desktop and in the start menu.

The installer (`/apps/installer.app`) takes four steps: the disk, how to
install, a last look, and the copying. The USB drive ICDA runs from is shown
but cannot be chosen.

- **Alongside** what is on the disk, in its largest free space (a GPT disk,
  started in UEFI mode, with 1 GB free at least). A slider sets ICDA's size,
  and a bar shows the disk as it will be. An existing EFI partition is
  reused, else a 260 MB one is made.
- **Erase the disk**: ICDA's own layout (EFI, a 512 MB swap partition, and
  ICDA). The install button stays off until "Yes, erase everything on ..."
  is ticked.
- **Choose partitions myself** opens Disk Utility's manual install for that
  disk.

The last look lists what will happen, with the disk as it is and as it will
be. While copying, the installer shows the stage, the share done and the
time taken; at the end, **Restart now**. On its first start the installed
ICDA asks for the first account (below). The arrow keys, Tab, Enter and
Escape work throughout, and Disk Utility's Install ICDA opens the same guide
for the disk it shows.

## Users

ICDA has user accounts. On the first start the desktop asks for the first
account: it is the computer's owner and an administrator, and it keeps the
existing `/home` and `/cfg`, so nothing moves. Leave its password empty to
sign in automatically. Add more accounts in
**Settings > Users**, each either **Standard** or **Administrator**; each change
asks for your own password. Every account gets a recovery key, shown once when
it is made (and renewable in Settings > Users): **Forgot password?** on the
sign-in screen takes it and sets a new password, and administrators can also
set other users' passwords. **Super+L**
locks the screen, and the start menu's footer has Lock and Sign out. The text
console (Ctrl+Alt+F2) asks for a name and password too.

Each user sees their own home as `/home` and their own settings as `/cfg`
(later accounts live in `/users/<name>`), so apps and Linux programs work
unchanged. The kernel (`kernel/proc/accounts.c`, `kernel/fs/vfs.c`) decides
what a signed-in user may do: other homes cannot be entered; `/apps`, `/bin`,
`/etc` and the rest of the system are read-only; `/tmp` files belong to whoever
made them (it is emptied at each sign-in); USB disks are open to everyone, the
system partition can be read by all and changed by administrators, and other
internal disks (Windows) are for administrators only. Formatting, installing,
disk editing, account changes and stopping other users' programs need an
administrator. Linux programs see the real user id and an `/etc/passwd` with
the accounts. Notifications are per user (the system's are everyone's). Any
user can install the assistant's models: `/sbin/model-install` runs as the
system, fetching only the catalog's pinned models and checking their SHA-256.
Passwords and recovery keys are PBKDF2-HMAC-SHA256 (200,000 rounds, older
hashes strengthened at the next sign-in) with per-account salts, kept in
`/etc/icda/users`, which only the kernel reads. `make vfs-users-test` checks
the rules on the host.

## Storage

Since 1.9, `/` is ICDA's own FAT32 partition, holding ordinary files:
`/apps`, `/bin`, `/etc`, `/usr`, `/linux`, `/home`, `/cfg` and so on. The boot
files (GRUB, `KERNEL.BIN`, `GRUBENV` and the previous kernel kept for a
rollback) are on the EFI partition, in `\EFI\ICDA` and `\UPDATE\BACKUP`.
Folders are listed when they are first opened and files are read when a
program opens them. Unused file contents are dropped from memory after 30
seconds, and sooner if the cache passes 128 MB. A write goes straight to the
disk. `/dev`, `/tmp` and `/volumes` live only in memory, and other disks
appear under `/volumes`.

Earlier versions kept the whole system in memory, loaded from one image,
`ICDAROOT.BIN`. The first 1.9 boot moves such an install to real files: it
unpacks the image onto the partition, moves `\SYSTEM` to `/`, moves the kernel
to the EFI partition and keeps the old image as `/ICDAROOT.OLD` (which can be
deleted later). If an update is still on trial, the move waits until the next
boot. An installation that has been moved can no longer be booted by a kernel
older than 1.9. The live USB still runs from memory. `make vfs-rootfs-test`
checks the loading and freeing on the host.

## Swap

When memory runs short, the pages programs have not used for a while are
written to a swap area and their memory goes to whoever needs it; a program
that touches such a page waits while it is read back. A program that needs
more than the computer's memory slows down instead of being stopped. Cached
file contents are dropped before anything is swapped. The window manager's
memory is never swapped, and it does not wait for the disk unless memory has
run out, so the desktop keeps answering while the disk works. When memory
and swap have both run out, the program that needs more is stopped, as
before.

The swap area is the first of:

- ICDA's own swap partition: the 512 MB **ICDA Swap** partition its disk
  layout makes (or one the installer was given for swap), on ICDA's disk;
- a swap partition Linux's `mkswap` prepared. Its first page, with
  Linux's signature, is left as it is, and one holding a hibernated system
  is not touched;
- `/swapfile` on ICDA's partition, made at the first start: as large as the
  memory (rounded up to 256 MB), from 1 GB to just under 4 GB, but never so
  large that less than 2 GB, or less than half of the free space, would be
  left. Not on a USB disk.

`/etc/icda/swap` holds `auto` (the default), `off`, or a size in MB, which
makes a swap file of that size the swap area. The swap file is made again
only when a different size is wanted, and removed when swap is off or a
partition is used. No program can read `/swapfile`, administrators
included, since other programs' memory is in it, and while swap is on, the
disk it is on cannot be formatted or repartitioned. The live USB uses no swap
unless `icda.swap=on` is on the kernel's command line (then a swap
partition); `icda.swap=off` turns swap off for one start. `/proc/meminfo`
shows `SwapTotal` and `SwapFree`, and a Linux program's `/proc/<pid>/status`
its `VmSwap`; a program's memory in Activity and `/proc` counts only what is
in memory. If the disk fails while a page is read back, only the program
that owned it is stopped. `make swapfile-test` checks the making, reuse
and removal of `/swapfile` on the host; nptestlx (`icda.test=nptestlx`, with
a swap partition) checks swapping with fork, mprotect, mremap and munmap.

## Windows programs

ICDA runs 64-bit Windows programs (`.exe`) itself, as it runs Linux ones:
starting one from Terminal, or from a program by any means (a Linux
program's `execve` included), runs it. The kernel hands a Windows program
to `/usr/bin/icda-win` in the Linux root, as it hands a script to its
interpreter, and `icda-win`, a Linux program, loads it and gives it the
Windows it expects: its threads have their TEB at GS (the kernel keeps
each thread's GS base), and the DLLs it imports are ICDA's own (kernel32
and ntdll, the C runtimes from msvcrt to the UCRT and vcruntime140, user32,
gdi32, opengl32, winmm, advapi32 with the registry, shell32, ole32,
ws2_32, bcrypt, cabinet, version and more) or the program's own, from its
folder. System DLLs ICDA has nothing of load as stubs where a program only
links to them, and fail to load where it asks for them, so it can manage
without. A function the layer does not have fails the way its callers
check for: `apis_gen.h`, made by `userspace/winlayer/gen_apis.py` from
MinGW-w64's headers, says what each of about 10,000 Windows functions
returns (FALSE with an error, NULL, `INVALID_HANDLE_VALUE`, `E_NOTIMPL`, an
NTSTATUS or a Win32 error code), and gives the names behind the export
numbers programs import `oleaut32`, `ws2_32`, `comctl32`, `shlwapi` and
`shell32` functions by.

The ANSI code page is UTF-8 (65001), so the text of the ANSI ("A")
functions is UTF-8: a program written for them reads and writes names in
any language, and their counts are bytes. Every A function whose Unicode
("W") version the layer has is there too (`ansi.c`, or beside the W one
where it needs the DLL's own state), each keeping Windows' rule for a
buffer that is too small (the size needed, or a cut, or
`ERROR_INSUFFICIENT_BUFFER`, as that function does). Named events,
mutexes, semaphores and waitable timers are found by their names while a
handle to them is open, so a program that checks whether it is running
already (a named mutex and `ERROR_ALREADY_EXISTS`) finds out; dates and
times are written from Windows' pictures (`dddd, MMMM d, yyyy`, `h:mm tt`)
in US English; and `SystemParametersInfo` answers as Windows 10 does at
96 dpi, its interface fonts Segoe UI at 9 points.

`C:` is `~/.windows/c` (`ICDA_WIN_PREFIX` names another folder) and `Z:` is
`/`, with names found whatever their case. The registry is
`C:\Windows\registry.txt`, shared by every program using the drive. A
program's window is an ICDA window: it gets the keys and the mouse as
Windows messages, raw input (`WM_INPUT`) for a game's mouse look (when a
game reads the raw mouse and hides its pointer, the window manager hands it
the motion and keeps the pointer out of sight), and `MessageBox` is an ICDA
dialog. GDI draws bitmaps, rectangles and text, in ICDA's typefaces.
DirectWrite, the font system programs such as Chrome draw their text with,
reads TrueType and OpenType fonts with FreeType: ICDA's own, the Linux
root's (DejaVu) and any put in the prefix's `C:\Windows\Fonts`. Windows'
family names (Segoe UI, Arial, Times New Roman, Consolas ...) find the
nearest family there is, and characters a font lacks fall back to one that
has them. Its text layout and text analyzer are not there.
OpenGL, up to 4.5, is Mesa's llvmpipe drawing into the window; what it
compiles of a program's shaders is kept in `~/.windows/cache/mesa`, so a
game stutters only the first time it shows something. Anisotropic
filtering is capped at 1 (plain trilinear filtering; `ICDA_WIN_ANISO=16`
allows it again): llvmpipe takes a texture sample for each step of a
surface's slant, so floors and water seen at a low angle cost several
times as much, and a surface seen edge-on took it nearly two minutes for
one frame. Sound (waveOut) plays
through ICDA's audio. Programs start others (`CreateProcess`, with pipes,
named pipes and 32-bit exit codes) and use the network through Winsock.
When a program stops on a fault, a dialog says where (the module and
offset), and `ICDA_WIN_TRACE=1` prints what it loads and calls of note
(`2`: also every call to something not implemented; `3`: also the first
call of each function the layer has, a short list of what a program uses).
With tracing on, `kill -QUIT` (Ctrl+\\ in a terminal) prints where each of
the program's threads is: the program's frames, or the layer function it
waits in and the frames that called it, for a program that hangs or does
nothing. `ICDA_WIN_FPS=1`
prints the frame rate every five seconds, and `ICDA_WIN_SLOW=<ms>` each
frame that took longer.

What was tried: the Quake engine Ironwail (GPL) with the free LibreQuake
game plays on ICDA (menus, console, maps, mouse look and keys); its
timedemo of LibreQuake's first demo, at 960x540 on four cores, runs at 43
frames a second (11 before anisotropic filtering was capped, with one frame
of 108 seconds), and with the compiled shaders kept its slowest frame takes
0.2 seconds instead of 1.1. 7-Zip 25.01's command line (`7z.exe` with
`7z.dll`, and `7za.exe`) makes, lists, tests and extracts archives (7z, zip,
tar.gz, AES with a password), which Linux's 7-Zip reads back unchanged, and
its benchmark rates 19,360 MIPS on four cores, where Alpine's Linux 7-Zip
rates 14,296. Chrome's x64 offline installer installs Chrome, and Chrome
158 starts: it opens its window, and with `--single-process` it loads a
page (the window's title becomes the page's), but draws nothing yet, since
it finds no graphics to draw with (no Direct3D, and its ANGLE does not come
as DLLs); in its usual form, its renderer and GPU processes do not start,
as handles do not pass to other processes yet. Not there: 32-bit programs,
Direct3D (games need an OpenGL renderer), .NET, the standard controls
(comctl32 has `InitCommonControlsEx` and window subclassing, but buttons,
edit fields and lists in a program's window are not drawn) and handles
inherited by or duplicated into another process.

`icda-win` ships with ICDA: `tests/linux/mkroot.sh` builds it (with
`userspace/winlayer/build.sh`, in Docker, against Alpine's Mesa) when it is
not built yet and puts it in every Linux root with the libraries it needs
(OSMesa and libmspack, 14 MB beside WebKit's), so the USB stick's `linux`
folder and every install have it. `scripts/ota-release.sh` will not publish
a release without it, and ships it as a file of its own, so an update to the
Windows layer does not download the WebKit pack again. `tests/winlayer/run.sh` builds test programs with
MinGW-w64 and checks them under `icda-win` on Linux (the C runtimes,
threads and waits, files, the registry, processes, version resources,
OpenGL, keys, GDI text, DirectWrite and the A functions); the programs, left in `tests/winlayer/out`,
run on ICDA too. In QEMU without KVM, give the guest `-cpu max,-avx,-avx2`:
QEMU 8.2 emulates some AVX2 instructions wrongly, and llvmpipe's 2D
drawing then goes wrong (an empty menu, a grey sky) or faults.

`tests/winlayer/conformance.sh <winetest64.exe>` measures the layer
against a conformance suite: Wine's test programs for each Windows DLL
(`winetest64.exe` bundles them; Wine's CI publishes it daily), run under
`icda-win` one test at a time and expected to pass as on Windows. It
writes a scoreboard per DLL (tests passed, failed, crashed, or still
running after the timeout, and checks failed) to
`tests/winlayer/out/conformance`. For the layer's 22 DLLs, 215 tests: 18
pass, 70 fail some checks, 110 crash and 17 do not finish, with 3,201,217
checks run; the crashes are the work list (the standard controls, dialogs
and menus among them).

A bad pointer a program passes to the layer fails the call, as on Windows,
where the layer's code would have faulted: its functions are entered
through a barrier (`barrier.c`) that notes the caller's state, and a fault
in the layer's code at NULL or within an argument ends the call with the
function's own failure (from `apis_gen.h`) and `ERROR_NOACCESS`. It costs
about 30 ns a call; locks, TLS, heaps, the C runtimes and OpenGL are
called directly. `ICDA_WIN_BARRIER=0` turns it off.

Exceptions are dispatched as on x64 Windows (`except.c`): vectored
handlers first, then each frame's language handler, found from the
functions' unwind data, then the program's unhandled exception filter.
`RtlUnwindEx` runs the cleanups (`__finally`, destructors) of the frames it
leaves; a fault in a program's code (an access violation, a division by
zero) is an exception it may handle. C++ exceptions work both MinGW's way
(libgcc's handler) and MSVC's (`_CxxThrowException` and the FH3 frame
handler, `cxxeh.c`): 7-Zip, built with MSVC, reports a bad switch or a
damaged archive as it does on Windows. Programs built with Visual Studio
2019 and later use the compressed FH4 tables (`__CxxFrameHandler4`), read
too, and `dynamic_cast` and `typeid` work from MSVC's RTTI (a failed cast to
a reference throws `std::bad_cast`). Microsoft's own C++ library,
`msvcp140.dll`, which programs carry beside them, runs on the layer's C
runtime (its locale's names of days and months, stream buffers, the new
handler), and the Windows thread pool (work, timers, waits, cleanup groups)
is there for it and for `std::async`. Not yet: exceptions crossing frames of
the layer's own that were not entered through the barrier.

`tests/winlayer/apps.sh` checks real programs, downloaded: 7-Zip 25.01
(archives made and read back, Linux's 7-Zip agreeing; AES; a bad switch)
and exiv2 0.28 from conda-forge, built with Visual Studio 2022 (an error it
throws and catches; EXIF tags it writes into a JPEG and reads back, which
exiftool agrees with).
