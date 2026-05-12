$ErrorActionPreference = "Stop"

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptDir
$isoPath = Join-Path $repoRoot "kernel.iso"
$serialLog = Join-Path $repoRoot "qemu-serial.log"
$diskPath = Join-Path $repoRoot "icda-disk.img"

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

$qemuArgs = @(
    "-machine", "pc"
    "-smp", "4"
    "-m", "4G"
    "-cdrom", $isoPath
    "-drive", "file=$diskPath,format=raw,if=ide,index=0,media=disk"
    "-boot", "d"
    "-serial", "file:$serialLog"
    "-no-reboot"
)

Write-Host "Launching QEMU with:"
Write-Host "  ISO: $isoPath"
Write-Host "  CPUs: 4"
Write-Host "  RAM: 4G"
Write-Host "  Disk: $diskPath"
$qemuPath = if ($qemuCmd.PSObject.Properties.Match("Source").Count -gt 0) { $qemuCmd.Source } else { $qemuCmd.FullName }
Write-Host "  QEMU: $qemuPath"
Write-Host "  Serial log: $serialLog"

& $qemuPath @qemuArgs
