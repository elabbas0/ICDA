param(
    [switch]$UseWsl,
    [switch]$NoWsl
)

$ErrorActionPreference = "Stop"

$DockerInstallerUrl = "https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe"
$UseHyperV = -not $UseWsl

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-WindowsFeatureEnabled {
    param([Parameter(Mandatory = $true)][string]$FeatureName)

    try {
        $feature = Get-WindowsOptionalFeature -Online -FeatureName $FeatureName -ErrorAction Stop
        return $feature.State -eq "Enabled"
    } catch {
        return $false
    }
}

function Remove-StaleDockerDesktopProgramData {
    $dockerDesktopData = Join-Path $env:ProgramData "DockerDesktop"
    if (-not (Test-Path -LiteralPath $dockerDesktopData)) {
        return
    }

    Write-Host ""
    Write-Host "Removing stale Docker Desktop data folder: $dockerDesktopData"
    Write-Host "This fixes Docker installer exit code -5 when the folder is not owned by an elevated account."

    Get-Process |
        Where-Object { $_.ProcessName -like "Docker*" -or $_.ProcessName -like "com.docker*" } |
        Stop-Process -Force -ErrorAction SilentlyContinue

    $adminsSid = "*S-1-5-32-544"
    & takeown.exe /F $dockerDesktopData /R /D Y | Out-Host
    & icacls.exe $dockerDesktopData /grant "${adminsSid}:(OI)(CI)F" /T /C | Out-Host

    Remove-Item -LiteralPath $dockerDesktopData -Recurse -Force
    if (Test-Path -LiteralPath $dockerDesktopData) {
        throw "Could not remove $dockerDesktopData. Close Docker-related windows and retry from an elevated shell."
    }
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget is required. Install App Installer from Microsoft Store, then re-run this script."
}

if ($UseHyperV -and -not (Test-Admin)) {
    throw "The Hyper-V no-WSL setup must be run from an elevated PowerShell window."
}

Write-Host "Installing Git with winget..."
winget install --exact --id Git.Git --accept-package-agreements --accept-source-agreements

Write-Host ""
if ($UseHyperV) {
    Write-Host "Enabling Hyper-V Docker Desktop requirements..."
    $hyperVResult = Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All -NoRestart
    $containersResult = Enable-WindowsOptionalFeature -Online -FeatureName Containers -All -NoRestart

    $hyperVReady = Test-WindowsFeatureEnabled "Microsoft-Hyper-V-All"
    $containersReady = Test-WindowsFeatureEnabled "Containers"
    $restartNeeded = $hyperVResult.RestartNeeded -or $containersResult.RestartNeeded

    if ($restartNeeded -or -not ($hyperVReady -and $containersReady)) {
        Write-Host ""
        Write-Host "Hyper-V/Containers were enabled, but Windows must reboot before Docker can install in Hyper-V mode."
        Write-Host "After reboot, run:"
        Write-Host "  scripts\install-requirements.cmd"
        Write-Host "  scripts\check-requirements.cmd"
        exit 3010
    }

    Remove-StaleDockerDesktopProgramData

    $installer = Join-Path $env:TEMP "DockerDesktopInstaller.exe"
    Write-Host ""
    Write-Host "Downloading Docker Desktop installer..."
    Invoke-WebRequest -Uri $DockerInstallerUrl -OutFile $installer

    Write-Host ""
    Write-Host "Installing Docker Desktop for all users with Hyper-V backend..."
    $dockerInstall = Start-Process -FilePath $installer -Wait -PassThru -ArgumentList @(
        "install",
        "--backend=hyper-v",
        "--always-run-service",
        "--accept-license"
    )
    if ($dockerInstall.ExitCode -ne 0) {
        throw "Docker Desktop installer failed with exit code $($dockerInstall.ExitCode)."
    }

    $currentUser = "$env:USERDOMAIN\$env:USERNAME"
    Write-Host ""
    Write-Host "Adding $currentUser to docker-users group..."
    net localgroup docker-users "$currentUser" /add | Out-Host

    Write-Host ""
    Write-Host "Hyper-V mode is configured. Docker Desktop may still need a restart or first launch."
} else {
    Write-Host "Installing Docker Desktop with winget..."
    winget install --exact --id Docker.DockerDesktop --accept-package-agreements --accept-source-agreements

    Write-Host "WSL2 is required for Docker Desktop's default backend."
    Write-Host "If WSL is not installed yet, open PowerShell as Administrator and run:"
    Write-Host "  wsl --install"
}
Write-Host ""
Write-Host "After any requested reboot, start Docker Desktop and run:"
if ($NoWsl) {
    Write-Host "  .\scripts\check-requirements.ps1 -NoWsl"
} else {
    Write-Host "  .\scripts\check-requirements.ps1"
}
Write-Host "  .\scripts\docker-smoke.ps1"
