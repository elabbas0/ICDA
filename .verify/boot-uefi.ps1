param([string]$Disk = ".verify/disk-win.img", [string]$Machine = "q35", [string]$Iso = "", [int]$Wait = 30, [switch]$FreshVars)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
if ($FreshVars -or -not (Test-Path .verify/ovmf-vars.fd)) { Copy-Item .verify/ovmf-vars-template.fd .verify/ovmf-vars.fd -Force }
$args = @("-machine", $Machine, "-smp", "2", "-m", "2G",
          "-drive", "if=pflash,format=raw,readonly=on,file=.verify/ovmf-code.fd",
          "-drive", "if=pflash,format=raw,file=.verify/ovmf-vars.fd",
          "-drive", "file=$Disk,format=raw,media=disk",
          "-display", "none", "-monitor", "none", "-serial", "file:.verify/serial.log",
          "-qmp", "tcp:127.0.0.1:4444,server,nowait", "-no-reboot")
if ($Iso) { $args += @("-cdrom", $Iso) }
Start-Process -FilePath "C:\Users\elabbas\tools\qemu\qemu-system-x86_64.exe" -ArgumentList $args -WindowStyle Hidden
Start-Sleep -Seconds $Wait
