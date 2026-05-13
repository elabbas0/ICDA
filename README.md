# ICDA
ICDA (short for I Can Do Anything) is a modular AI-based operating system. Fully customizable without any code knowledge required

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
