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
**Local**, and click **Model** to choose Qwen 2.5 0.5B (469 MiB) or 1.5B
(1066 MiB). Click **Download from Hugging Face** to download only that model.
Progress appears in the Assistant pane. The selected file is streamed from
Qwen's official pinned HTTPS URL to ICDA's own partition, checked against its
expected byte size and SHA-256 hash, and renamed into place after verification.
Existing model files are preserved on failure. No restart is required.
Interrupted connections reconnect and resume with HTTP ranges. The model still
needs enough free RAM to run; insufficient memory is reported in the assistant.

The assistant also supports `/model install 0.5b`, `/model install 1.5b`,
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
