# ICDA Requirements

ICDA can be built in two supported ways:

1. Docker path: recommended for every developer machine.
2. Native Linux path: useful for CI images, WSL, or bare Linux boxes.

## Windows recommended setup

Run the all-in-one command:

```cmd
scripts\icda.cmd ready
```

Do not run `.ps1` files directly on Windows. Use `scripts\icda.cmd`; it invokes
PowerShell with `-ExecutionPolicy Bypass` for this repo command.

The default Windows path is automatic backend selection. The command checks
requirements, starts the elevated setup when needed, tries Docker Desktop with
WSL2 first, and falls back to Hyper-V if the WSL setup path fails during
installation. It then builds the Docker image, builds `kernel.iso`, and runs
the QEMU smoke test.

Docker Desktop currently requires Windows 10 22H2 build 19045 or newer, or a
supported Windows 11 install. Git Bash is only a shell; it cannot replace WSL2
or Hyper-V as Docker's Linux container backend.

If either WSL2 or Hyper-V has to be enabled, Windows may need a reboot before
Docker Desktop can be installed. The setup registers an automatic resume task
named `ICDA Docker Desktop Setup Resume`, reboots Windows, and continues the
selected backend setup after you log back in. After Docker Desktop's first
launch, run `scripts\icda.cmd ready` again to build and smoke-test the OS.

Useful focused commands:

```cmd
scripts\icda.cmd check
scripts\icda.cmd install
scripts\icda.cmd build
scripts\icda.cmd smoke
scripts\icda.cmd qemu
scripts\icda.cmd ready -UseWsl
scripts\icda.cmd ready -NoWsl
```

## Windows WSL path

If you want to force Docker Desktop's WSL backend, pass `-UseWsl`:

```cmd
scripts\icda.cmd ready -UseWsl
```

## Windows without direct PowerShell scripts

Use the `.cmd` entry point. It runs PowerShell with `-ExecutionPolicy Bypass`
for this repo command only:

```cmd
scripts\icda.cmd ready
```

The repo keeps the user-facing script surface small: use `scripts\icda.cmd`
on Windows and `./scripts/icda.sh` from POSIX shells.

## Native Linux setup

Install the native build and test packages:

```sh
sudo ./scripts/install-requirements.sh
```

Then build and test:

```sh
make
make qemu-smoke
```

Or use Docker on Linux:

```sh
./scripts/icda.sh ready
```

## Docker image tools

The Docker image installs:

- `gcc`
- `make`
- `nasm`
- `grub-mkrescue`
- `xorriso`
- `mtools`
- `qemu-system-x86_64`

Host machines only need Docker for the normal workflow.
