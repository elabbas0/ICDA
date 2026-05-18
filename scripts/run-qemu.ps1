[CmdletBinding()]
param(
    [switch]$Headless,
    [switch]$Uefi
)

$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$isoPath = Join-Path $repoRoot "kernel.iso"
$serialLog = if ($Headless) {
    Join-Path $repoRoot "qemu-serial.log"
} else {
    Join-Path $repoRoot ("qemu-serial-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
}
$diskPath = Join-Path $repoRoot "icda-disk.img"
$serialLogQemu = ($serialLog -replace '\\','/')

if (Test-Path -LiteralPath $serialLog) {
    Remove-Item -LiteralPath $serialLog -Force -ErrorAction SilentlyContinue
}

if (-not (Test-Path -LiteralPath $isoPath)) {
    throw "kernel.iso not found at $isoPath"
}

$qemuCmd = Get-Command qemu-system-x86_64.exe -ErrorAction SilentlyContinue
if (-not $qemuCmd) {
    $qemuCmd = Get-Command qemu-system-x86_64 -ErrorAction SilentlyContinue
}

if (-not $qemuCmd) {
    $commonQemuPaths = @(
        "C:\Program Files\qemu\qemu-system-x86_64.exe",
        "C:\Program Files\QEMU\qemu-system-x86_64.exe"
    )

    foreach ($candidate in $commonQemuPaths) {
        if (Test-Path -LiteralPath $candidate) {
            $qemuCmd = Get-Item -LiteralPath $candidate
            break
        }
    }
}

if (-not $qemuCmd) {
    throw "qemu-system-x86_64 was not found on PATH or in the default QEMU install folders"
}

if (-not (Test-Path -LiteralPath $diskPath)) {
    $stream = [System.IO.File]::Open($diskPath, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
    try {
        $stream.SetLength(128MB)
    } finally {
        $stream.Dispose()
    }
}

$qemuPath = if ($qemuCmd.PSObject.Properties.Match("Source").Count -gt 0) { $qemuCmd.Source } else { $qemuCmd.FullName }
$qemuDir = Split-Path -Parent $qemuPath
$qemuShare = Join-Path $qemuDir "share"
$ovmfCode = Join-Path $qemuShare "edk2-x86_64-code.fd"
$ovmfVarsSource = Join-Path $qemuShare "edk2-i386-vars.fd"
$ovmfVars = Join-Path $repoRoot "qemu-uefi-vars.fd"

$machineArg = "pc"
if (-not $Headless) {
    $machineArg = "pc,pcspk-audiodev=audio0"
}

$qemuArgs = @(
    "-machine", $machineArg
    "-smp", "4"
    "-m", "4G"
    "-cdrom", $isoPath
    "-drive", "file=$diskPath,format=raw,if=ide,index=0,media=disk"
    "-boot", "d"
    "-serial", "file:$serialLogQemu"
    "-no-reboot"
)

if ($Headless) {
    $qemuArgs += @("-display", "none", "-monitor", "none")
} else {
    $qemuArgs += @("-audiodev", "sdl,id=audio0", "-device", "sb16,audiodev=audio0")
}

if ($Uefi) {
    if (-not (Test-Path -LiteralPath $ovmfCode)) {
        throw "UEFI firmware was not found at $ovmfCode"
    }
    if (-not (Test-Path -LiteralPath $ovmfVars)) {
        if (-not (Test-Path -LiteralPath $ovmfVarsSource)) {
            throw "UEFI vars template was not found at $ovmfVarsSource"
        }
        Copy-Item -LiteralPath $ovmfVarsSource -Destination $ovmfVars
    }
    $qemuArgs += @(
        "-drive", "if=pflash,format=raw,readonly=on,file=$ovmfCode"
        "-drive", "if=pflash,format=raw,file=$ovmfVars"
    )
}

Write-Host "Launching QEMU with:"
Write-Host "  ISO: $isoPath"
Write-Host "  CPUs: 4"
Write-Host "  RAM: 4G"
Write-Host "  Disk: $diskPath"
Write-Host "  QEMU: $qemuPath"
Write-Host "  Serial log: $serialLog"
if ($Uefi) {
    Write-Host "  Firmware: $ovmfCode"
}

& $qemuPath @qemuArgs
