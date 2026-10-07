param([int]$Wait = 30)
# live ISO + persistence disk + FAT32 media test disk, HDA audio recorded to audio-out.wav
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
Get-Process qemu-system-x86_64 -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500
$a = @("-machine","q35","-smp","2","-m","4G","-cdrom","kernel.iso","-boot","d",
 "-drive","file=.verify/disk.img,format=raw,media=disk",
 "-drive","file=.verify/mediadisk.img,format=raw,media=disk",
 "-nic","user,model=e1000",
 "-audiodev","wav,id=snd0,path=.verify/audio-out.wav","-device","ich9-intel-hda","-device","hda-output,audiodev=snd0",
 "-display","none","-monitor","none","-serial","file:.verify/serial.log",
 "-qmp","tcp:127.0.0.1:4444,server,nowait","-no-reboot")
Start-Process -FilePath "C:\Users\elabbas\tools\qemu\qemu-system-x86_64.exe" -ArgumentList $a -WindowStyle Hidden
Start-Sleep -Seconds $Wait
"900 700" | Set-Content -Encoding ascii (Join-Path $root "gui-cursor.txt")
powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Move "0,0" -StepMs 150 -SettleMs 300 | Out-Null
