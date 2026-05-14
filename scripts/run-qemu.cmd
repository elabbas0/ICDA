@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run-qemu.ps1"
endlocal
