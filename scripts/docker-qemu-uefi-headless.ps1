$ErrorActionPreference = "Stop"

. "$PSScriptRoot\docker-common.ps1"

Invoke-Docker @("build", "-t", "icda-toolchain", ".")
$ttyArgs = @()
if (-not [Console]::IsInputRedirected) {
    $ttyArgs = @("-it")
}

$runArgs = @("run", "--rm") + $ttyArgs + @("-v", "${PWD}:/workspace", "-w", "/workspace", "icda-toolchain", "make", "qemu-uefi-headless")
Invoke-Docker $runArgs
