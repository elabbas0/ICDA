param([string]$EfiIndex = "0", [string]$RootIndex = "2")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$q = "scripts\qmp-input.ps1"
docker run --rm -v "C:/Users/elabbas/Desktop/ICDA:/workspace" -w /workspace icda-toolchain sh .verify/mkwin.sh | Out-Null
powershell -ExecutionPolicy Bypass -File .verify\boot-uefi.ps1 -FreshVars -Iso kernel.iso -Wait 40 | Out-Null
"900 700" | Set-Content -Encoding ascii gui-cursor.txt
powershell -ExecutionPolicy Bypass -File $q -Move "0,0" -StepMs 150 -SettleMs 300 | Out-Null
powershell -ExecutionPolicy Bypass -File .verify\launch.ps1 -X 155 -Y 335 -Shot .verify\db0.png | Out-Null
foreach ($ch in "install".ToCharArray()) { powershell -ExecutionPolicy Bypass -File $q -Key "$ch" -SettleMs 100 | Out-Null }
powershell -ExecutionPolicy Bypass -File $q -Key "ret" -SettleMs 2500 | Out-Null
foreach ($k in @($EfiIndex, "ret", $RootIndex, "ret", "minus", "1", "ret")) { powershell -ExecutionPolicy Bypass -File $q -Key $k -SettleMs 300 | Out-Null }
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline -and -not (Select-String -Path .verify\serial.log -Pattern "efi: registered|efi: SetVariable" -Quiet)) { Start-Sleep -Seconds 3 }
Start-Sleep -Seconds 5
Select-String -Path .verify\serial.log -Pattern "efi:" | ForEach-Object { $_.Line }
powershell -ExecutionPolicy Bypass -File .verify\boot-uefi.ps1 -Wait 3 | Out-Null
for ($i = 0; $i -lt 5; $i++) { powershell -ExecutionPolicy Bypass -File $q -Shot ".verify\db-grub$i.png" -SettleMs 50 | Out-Null; Start-Sleep -Milliseconds 800 }
Start-Sleep -Seconds 25
powershell -ExecutionPolicy Bypass -File $q -Shot ".verify\db-desktop.png" -SettleMs 50 | Out-Null
Select-String -Path .verify\serial.log -Pattern "BdsDxe: starting" | ForEach-Object { $_.Line -replace '\x1b\[[0-9;=]*[A-Za-z]','' }
