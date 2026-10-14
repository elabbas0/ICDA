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

The assistant can also run the official OpenAI Codex CLI with a ChatGPT
account. Prepare its pinned, SHA-256-verified Linux package using
`powershell -ExecutionPolicy Bypass -File scripts/prepare-codex.ps1`.
The executable is installed in ICDA's own `linux/usr/bin/codex`, together
with the official tool helpers and resources; no local
OpenAI model weights or Windows partition are needed. The CLI software
comes from OpenAI's release; Qwen model weights still download directly
from official Hugging Face URLs.

In Settings > Assistant, add a provider and choose **Codex**. Click **Sign
in with ChatGPT** to open the assistant with the device sign-in link and
one-time code. Device login must be enabled in your ChatGPT security
settings. Enter `auto` in Model for the account default, or an available
Codex model ID. You can also type `/codex login`, `/codex status`,
`/codex version`, or `/codex <task>` in the assistant.

Codex receives the current task handoff and BusyBox tool instructions in
`/home/icda/AGENTS.md` and `CURRENT_TASK.md`. This is a new Codex session
using shared context. This owner's integration grants system tool access;
the CLI uses `danger-full-access` because ICDA lacks Linux sandbox namespaces.
Credentials stay in ICDA's own `linux/var/lib/icda/codex` on the installed
system partition; never include them in an update bundle. The offline
updater can privately reuse an existing ChatGPT Codex sign-in with
`-ChatGPTAuthCache "$env:USERPROFILE/.codex/auth.json"`, using OpenAI's
documented auth-cache fallback. It preserves an existing ICDA sign-in.
Runtime diagnostics are in `/tmp/ai/codex.log`. Codex needs an internet
connection and account access; the local Qwen option remains offline.

The local assistant runs `icda-llm` with GGUF models kept on the ICDA system
partition, under `linux/usr/share/icda/models`. It does not search Windows
partitions for models. The default is Qwen2.5 0.5B Instruct Q4_K_M from
[Qwen's official repository](https://huggingface.co/Qwen/Qwen2.5-0.5B-Instruct-GGUF).

Prepare the runtime and model bundle on Windows:

```powershell
& 'C:/Program Files/Git/bin/bash.exe' userspace/llm/build.sh
powershell -ExecutionPolicy Bypass -File scripts/prepare-ai.ps1 -IncludeLargerModel
```

Omit `-IncludeLargerModel` for only the smaller model. Downloads use pinned
revisions and verified SHA-256 hashes. The optional Qwen2.5 1.5B model needs
more RAM. `scripts/build-deploy.sh` includes the prepared runtime and models;
the dual-boot installer copies them into ICDA's own `linux` directory. Existing
systems can copy the bundle's `linux` directory to their ICDA system partition.

In Settings > Assistant, select a local provider, use `file:auto` for Address,
and click Model to cycle installed GGUF files. Changes apply on the next
question. The runner restarts when the model changes. Diagnostics are in
`/tmp/ai/llm.log`; failures appear in the assistant instead of leaving it silent.

OTA releases contain signed model URLs and checksums. ICDA downloads model
weights directly over HTTP(S) from Qwen's official Hugging Face repositories;
model weights are excluded from the GitHub OTA payload. Existing updaters
first install the new downloader. After restarting, use Settings > Updates >
Check now to download the models, then restart to install them in ICDA.

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
