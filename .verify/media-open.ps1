param([string]$File, [int]$Wait = 10)
# opens a terminal and runs Media on a file from the media disk, then screenshots
powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Key "ctrl+alt+t" -SettleMs 3000 | Out-Null
powershell -ExecutionPolicy Bypass -File .verify\type.ps1 -Text "run /apps/media.app /volumes/fat32-2/$File" -Enter
Start-Sleep -Seconds $Wait
powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Move "400,300" -SettleMs 300 | Out-Null
powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Shot .verify\shot.png -SettleMs 800 | Out-Null
