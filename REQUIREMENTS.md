# ICDA Requirements

ICDA can be built in two supported ways:

1. Docker path: recommended for every developer machine.
2. Native Linux path: useful for CI images, WSL, or bare Linux boxes.

## Windows recommended setup

Install these host requirements for the default path:

- Windows Subsystem for Linux 2
- Docker Desktop
- Git

Check the machine:

```powershell
.\scripts\check-requirements.ps1
```

Install the Windows requirements with winget and WSL:

```powershell
.\scripts\install-requirements.ps1 -UseWsl
```

After Docker Desktop starts successfully:

```powershell
.\scripts\docker-build.ps1
.\scripts\docker-qemu.ps1
.\scripts\docker-smoke.ps1
```

## Windows without WSL

On Windows Pro/Enterprise/Education, Docker Desktop can use Hyper-V instead of
WSL2. This project uses Docker's all-users install with `--backend=hyper-v`,
because Docker's per-user install only supports the WSL 2 backend.

Check the Hyper-V path:

```powershell
.\scripts\check-requirements.ps1 -NoWsl
```

Install the Hyper-V path from an elevated PowerShell. This is the default for
this repo's Windows setup script:

```powershell
.\scripts\install-requirements.ps1
```

Or launch the elevated setup from a normal PowerShell:

```powershell
.\scripts\start-hyperv-setup.ps1
```

If `.ps1` files open in Notepad, run the command wrappers instead:

```cmd
scripts\check-requirements.cmd
scripts\start-hyperv-setup.cmd
scripts\install-requirements.cmd
```

After any reboot, open Docker Desktop. Then run:

```powershell
.\scripts\check-requirements.ps1 -NoWsl
.\scripts\docker-smoke.ps1
```

The `.cmd` wrappers and `install-requirements.ps1` default to the no-WSL
Hyper-V path. Pass `-UseWsl` only if you intentionally want Docker's WSL
backend.

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

## Tools provided by Docker

The Docker image installs:

- `gcc`
- `make`
- `nasm`
- `grub-mkrescue`
- `xorriso`
- `mtools`
- `qemu-system-x86_64`

That means host machines only need Docker for the normal workflow.
