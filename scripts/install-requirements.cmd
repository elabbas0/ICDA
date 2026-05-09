@echo off
setlocal
if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-requirements.ps1"
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install-requirements.ps1" %*
)
