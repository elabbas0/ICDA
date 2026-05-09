param(
    [switch]$NoWsl
)

$ErrorActionPreference = "Stop"

function Test-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

function Write-Check {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][bool]$Ok,
        [string]$Hint = ""
    )

    if ($Ok) {
        Write-Host "[OK]   $Name"
    } else {
        Write-Host "[MISS] $Name"
        if ($Hint) {
            Write-Host "       $Hint"
        }
    }
}

$wingetOk = Test-Command "winget"
$dockerOk = Test-Command "docker"
$gitOk = Test-Command "git"
$dockerAllUsersInstall = Test-Path "C:\Program Files\Docker\Docker\Docker Desktop.exe"

function Test-WindowsFeatureEnabled {
    param([Parameter(Mandatory = $true)][string]$FeatureName)

    try {
        $feature = Get-WindowsOptionalFeature -Online -FeatureName $FeatureName -ErrorAction Stop
        return $feature.State -eq "Enabled"
    } catch {
        return $false
    }
}

$wslInstalled = $false
if (-not $NoWsl) {
    if (Test-Command "wsl") {
        $previousErrorActionPreference = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $wslOutput = (& wsl --status 2>&1) -join "`n"
        $ErrorActionPreference = $previousErrorActionPreference
        $wslInstalled = $LASTEXITCODE -eq 0 -and $wslOutput -notmatch "is not installed"
    }
}

$hyperVEnabled = Test-WindowsFeatureEnabled "Microsoft-Hyper-V-All"
$containersEnabled = Test-WindowsFeatureEnabled "Containers"

Write-Check "winget" $wingetOk "Install App Installer from Microsoft Store."
Write-Check "git" $gitOk "Run: winget install --exact --id Git.Git"
if ($NoWsl) {
    Write-Check "Hyper-V" $hyperVEnabled "Run: .\scripts\start-hyperv-setup.cmd"
    Write-Check "Windows Containers feature" $containersEnabled "Run: .\scripts\start-hyperv-setup.cmd"
    Write-Check "Docker Desktop all-users install" $dockerAllUsersInstall "Run: .\scripts\start-hyperv-setup.cmd"
} else {
    Write-Check "WSL2" $wslInstalled "Run from an elevated PowerShell: wsl --install"
}
if ($NoWsl) {
    Write-Check "Docker Desktop" $dockerOk "Run: .\scripts\start-hyperv-setup.cmd"
} else {
    Write-Check "Docker Desktop" $dockerOk "Run: winget install --exact --id Docker.DockerDesktop"
}

if ($dockerOk) {
    docker --version
}

if ($NoWsl) {
    $ok = $wingetOk -and $gitOk -and $hyperVEnabled -and $containersEnabled -and $dockerAllUsersInstall -and $dockerOk
} else {
    $ok = $wingetOk -and $gitOk -and $wslInstalled -and $dockerOk
}

if (-not $ok) {
    exit 1
}
