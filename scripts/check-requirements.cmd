@echo off
setlocal
if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-requirements.ps1" -NoWsl
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-requirements.ps1" %*
)
