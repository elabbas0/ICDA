<
.SYNOPSIS
  Minimal QMP client: send one human-monitor-command over QMP.

.DESCRIPTION
  scripts/gui-check.py needs Python, which is not installed on this host.
  This does the one thing we need to verify the GUI: ask a running QEMU
  (started with -qmp tcp:...,server,nowait -display none) to write a
  screendump of the guest framebuffer, then optionally convert it to PNG
  with scripts/ppm2png.ps1.

  A screendump is the only way to see the ICDA desktop: the kernel turns
  the serial mirror off once the framebuffer is up (kernel/kernel.c), so
  headless serial output stops at "[S22 shell]" whether or not the GUI
  came up.

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\qmp-screendump.ps1 -Port 4444 -Out shot.ppm

param(
    [int]$Port = 4444,
    [string]$Out = "gui-shot.ppm",
    [string]$Command = "",
    [int]$WaitSeconds = 0
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ($WaitSeconds -gt 0) {
    Write-Host "Waiting $WaitSeconds s for the guest to settle..."
    Start-Sleep -Seconds $WaitSeconds
}



$outAbs = if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path $RepoRoot $Out }
$outQemu = ($outAbs -replace '\\', '/') -replace ' ', '\ '
if (Test-Path -LiteralPath $outAbs) { Remove-Item -LiteralPath $outAbs -Force }

if (-not $Command) { $Command = "screendump $outQemu" }

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect("127.0.0.1", $Port)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    $writer.NewLine = "`n"

    function Send-Qmp([hashtable]$obj) {
        $json = $obj | ConvertTo-Json -Compress -Depth 6
        $writer.WriteLine($json)
        
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { throw "QMP connection closed" }
            $msg = $line | ConvertFrom-Json
            if ($msg.PSObject.Properties.Name -contains 'id' -and $msg.id -eq $obj.id) {
                if ($msg.PSObject.Properties.Name -contains 'error') {
                    throw "QMP error: $($msg.error.desc)"
                }
                return $msg
            }
        }
    }

    $greeting = $reader.ReadLine()
    if ($null -eq $greeting) { throw "no QMP greeting (is QEMU running with -qmp?)" }

    [void](Send-Qmp @{ execute = "qmp_capabilities"; id = 1 })
    $reply = Send-Qmp @{ execute = "human-monitor-command"; arguments = @{ "command-line" = $Command }; id = 2 }
    Write-Host "QMP reply: $($reply.return)"

    Start-Sleep -Milliseconds 800
    if (Test-Path -LiteralPath $outAbs) {
        Write-Host "captured $outAbs ($((Get-Item $outAbs).Length) bytes)"
        $png = [System.IO.Path]::ChangeExtension($outAbs, ".png")
        & (Join-Path $RepoRoot "scripts\ppm2png.ps1") -In $outAbs -Out $png
        exit 0
    }
    Write-Host "screendump produced no file: $Command"
    exit 1
} finally {
    $client.Close()
}
