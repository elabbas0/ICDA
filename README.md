# ICDA
ICDA (short for I Can Do Anything) is a modular AI-based operating system. Fully customizable without any code knowledge required

## Build and test

The easiest portable path is Docker. It provides the compiler, NASM, GRUB ISO
tools, and QEMU in one image.

Check host requirements first:

```powershell
.\scripts\check-requirements.ps1
```

For Windows without WSL, use the Hyper-V path:

```powershell
.\scripts\check-requirements.ps1 -NoWsl
.\scripts\start-hyperv-setup.ps1
```

If `.ps1` files open in Notepad, use the `.cmd` wrappers:

```cmd
scripts\check-requirements.cmd
scripts\start-hyperv-setup.cmd
scripts\install-requirements.cmd
scripts\docker-qemu.cmd
scripts\docker-qemu-headless.cmd
scripts\docker-qemu-uefi.cmd
scripts\docker-smoke.cmd
```

```sh
docker build -t icda-toolchain .
docker run --rm -v "$PWD:/workspace" -w /workspace icda-toolchain make
docker run --rm -v "$PWD:/workspace" -w /workspace icda-toolchain make qemu-smoke
```

On PowerShell:

```powershell
.\scripts\docker-build.ps1
.\scripts\docker-qemu.ps1
.\scripts\docker-qemu-uefi.ps1
.\scripts\docker-smoke.ps1
```

If the native toolchain is installed locally:

```sh
make
make qemu-smoke
make qemu
```
