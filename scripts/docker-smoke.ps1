$ErrorActionPreference = "Stop"

. "$PSScriptRoot\docker-common.ps1"

Invoke-Docker @("build", "-t", "icda-toolchain", ".")
Invoke-Docker @("run", "--rm", "-v", "${PWD}:/workspace", "-w", "/workspace", "icda-toolchain", "make", "qemu-smoke")
