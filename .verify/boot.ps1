param([string]$Iso = "kernel.iso", [int]$Wait = 25, [string]$Disk = ".verify/disk.img", [int]$Smp = 4)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$args = @("-machine", "q35", "-smp", "$Smp", "-m", "4G", "-cdrom", $Iso, "-boot", "d",
          "-drive", "file=$Disk,format=raw,media=disk",
          "-nic", "user,model=e1000",
          "-display", "none", "-monitor", "none", "-serial", "file:.verify/serial.log",
          "-qmp", "tcp:127.0.0.1:4444,server,nowait", "-no-reboot")
Start-Process -FilePath "C:\Users\elabbas\tools\qemu\qemu-system-x86_64.exe" -ArgumentList $args -WindowStyle Hidden
Start-Sleep -Seconds $Wait
"900 700" | Set-Content -Encoding ascii (Join-Path $root "gui-cursor.txt")
powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Move "0,0" -StepMs 150 -SettleMs 300
