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
The built-in assistant has no Codex provider or Codex commands.

Prepare the verified official package with
`powershell -ExecutionPolicy Bypass -File scripts/prepare-codex.ps1`.
The package lives in ICDA's `linux/usr`, credentials in its own
`linux/var/lib/icda/codex`, and temporary SQLite data in `/tmp/codex-state`.
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
