$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$installScript = Join-Path $PSScriptRoot "install-requirements.ps1"

Start-Process powershell.exe -Verb RunAs -ArgumentList @(
    "-NoProfile",
    "-ExecutionPolicy", "Bypass",
    "-File", "`"$installScript`""
) -WorkingDirectory $repoRoot

Write-Host "Started elevated Hyper-V/Docker setup. Accept the Windows UAC prompt to continue."
