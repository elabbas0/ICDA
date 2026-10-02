



param([switch]$HyperV)

$ErrorActionPreference = "Stop"

function Test-Admin {
    $p = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

if (-not (Test-Admin)) {
    Write-Host "Not elevated - relaunching as admin..."
    $args = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "`"$PSCommandPath`"")
    if ($HyperV) { $args += "-HyperV" }
    Start-Process powershell.exe -Verb RunAs -ArgumentList $args
    exit 0
}

Write-Host "Enabling VirtualMachinePlatform..."
Enable-WindowsOptionalFeature -Online -FeatureName VirtualMachinePlatform -All -NoRestart | Out-Null

Write-Host "Enabling Windows Subsystem for Linux..."
Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Windows-Subsystem-Linux -All -NoRestart | Out-Null

if ($HyperV) {
    Write-Host "Enabling Hyper-V + Containers (fallback path)..."
    Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All -NoRestart | Out-Null
    Enable-WindowsOptionalFeature -Online -FeatureName Containers -All -NoRestart | Out-Null
}

Write-Host ""
Write-Host "Features staged. Updating WSL..."
try { wsl.exe --update } catch { Write-Host "wsl --update not ready yet (expected before first reboot): $_" }

Write-Host ""
Write-Host "Done. REBOOT NOW, then in a normal shell run:"
Write-Host '  wsl --install        # first time only; reboot again if asked'
Write-Host '  wsl --status         # should print version, not "not installed"'
Write-Host '  # start Docker Desktop from Start menu, wait for green, then:'
Write-Host '  scripts\icda.cmd ready'
