param(
    [ValidateSet("ready", "check", "install", "repair", "repair-upgrade", "image", "build", "smoke", "qemu", "clean")]
    [string]$Action = "ready",
    [ValidateSet("auto", "wsl", "hyperv")]
    [string]$Mode = "auto",
    [switch]$UseWsl,
    [switch]$NoWsl,
    [switch]$Headless,
    [switch]$Uefi,
    [switch]$NoSmoke,
    [switch]$ResumeAfterReboot
)

$ErrorActionPreference = "Stop"

$RepoRoot = Split-Path -Parent $PSScriptRoot
$ImageName = "icda-toolchain"
$DockerInstallerUrl = "https://desktop.docker.com/win/main/amd64/Docker%20Desktop%20Installer.exe"
$Windows10UpgradeAssistantUrl = "https://go.microsoft.com/fwlink/?LinkID=799445"
$ResumeTaskName = "ICDA Docker Desktop Setup Resume"
$Script:SanitizedDockerConfigPath = $null

function Get-InstallMode {
    param(
        [string]$RequestedMode,
        [switch]$UseWslMode,
        [switch]$NoWslMode
    )

    if ($RequestedMode -eq "wsl") {
        return "wsl"
    }
    if ($RequestedMode -eq "hyperv") {
        return "hyperv"
    }
    if ($UseWslMode) {
        return "wsl"
    }
    if ($NoWslMode) {
        return "hyperv"
    }
    return "auto"
}

function Get-DockerCli {
    $cmd = Get-Command docker -ErrorAction SilentlyContinue
    if ($cmd) {
        return $cmd.Source
    }

    $programFilesDocker = "C:\Program Files\Docker\Docker\resources\bin\docker.exe"
    if (Test-Path $programFilesDocker) {
        return $programFilesDocker
    }

    throw "Docker CLI was not found. Run scripts\icda.cmd install, then start Docker Desktop and run scripts\icda.cmd ready."
}

function Invoke-Docker {
    param(
        [Parameter(ValueFromRemainingArguments = $true)]
        [object[]]$Arguments
    )

    $docker = Get-DockerCli
    if ($Arguments.Count -eq 1 -and $Arguments[0] -is [array]) {
        $Arguments = $Arguments[0]
    }

    $dockerArgs = @($Arguments | ForEach-Object { [string]$_ })
    $previousDockerConfig = $env:DOCKER_CONFIG
    $sanitizedDockerConfig = Get-SanitizedDockerConfigPath

    try {
        if ($sanitizedDockerConfig) {
            $env:DOCKER_CONFIG = $sanitizedDockerConfig
        }
        & $docker @dockerArgs
    } finally {
        if ($null -eq $previousDockerConfig -or $previousDockerConfig -eq "") {
            Remove-Item Env:DOCKER_CONFIG -ErrorAction SilentlyContinue
        } else {
            $env:DOCKER_CONFIG = $previousDockerConfig
        }
    }

    if ($LASTEXITCODE -ne 0) {
        throw "docker $($dockerArgs -join ' ') failed with exit code $LASTEXITCODE"
    }
}

function Invoke-CommandChecked {
    param(
        [Parameter(Mandatory = $true)]
        [string]$FilePath,
        [string[]]$Arguments = @(),
        [string]$FailureMessage = ""
    )

    & $FilePath @Arguments
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        if ($FailureMessage) {
            throw "$FailureMessage Exit code: $exitCode"
        }
        throw "$FilePath $($Arguments -join ' ') failed with exit code $exitCode"
    }
}

function Test-Admin {
    $principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-Command {
    param([Parameter(Mandatory = $true)][string]$Name)
    $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
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

function Test-DockerDesktopSupportedWindows {
    $build = [Environment]::OSVersion.Version.Build
    return $build -ge 19045
}

function Assert-DockerDesktopSupportedWindows {
    $build = [Environment]::OSVersion.Version.Build
    if ($build -lt 19045) {
        $message = "Docker Desktop requires Windows 10 22H2 build 19045 or newer. This PC is build $build. Run Windows Update first, then re-run scripts\icda.cmd ready."
        $exception = New-Object System.InvalidOperationException($message)
        throw $exception
    }
}

function Get-DockerDesktopUnsupportedMessage {
    $build = [Environment]::OSVersion.Version.Build
    return "Docker Desktop still requires Windows 10 22H2 build 19045 or newer. This PC is build $build. WSL can be installed now, but Docker Desktop must wait until after Windows Update."
}

function Get-SanitizedDockerConfigPath {
    if ($Script:SanitizedDockerConfigPath -and (Test-Path $Script:SanitizedDockerConfigPath)) {
        return $Script:SanitizedDockerConfigPath
    }

    $sourceDockerConfig = Join-Path $env:USERPROFILE ".docker"
    $sourceConfigJson = Join-Path $sourceDockerConfig "config.json"
    if (-not (Test-Path $sourceConfigJson)) {
        return $null
    }

    $config = Get-Content $sourceConfigJson -Raw | ConvertFrom-Json
    if (-not $config.PSObject.Properties["credsStore"]) {
        return $null
    }

    $targetDockerConfig = Join-Path $env:TEMP "icda-docker-config"
    if (Test-Path $targetDockerConfig) {
        Remove-Item -Recurse -Force $targetDockerConfig
    }

    Copy-Item -Recurse -Force $sourceDockerConfig $targetDockerConfig

    $targetConfigJson = Join-Path $targetDockerConfig "config.json"
    $targetConfig = Get-Content $targetConfigJson -Raw | ConvertFrom-Json
    $targetConfig.PSObject.Properties.Remove("credsStore")
    $targetConfig | ConvertTo-Json -Depth 10 | Set-Content -Path $targetConfigJson -Encoding ASCII

    $Script:SanitizedDockerConfigPath = $targetDockerConfig
    return $Script:SanitizedDockerConfigPath
}

function Get-SecConfigDeploymentManifestPath {
    return "C:\Windows\WinSxS\Manifests\amd64_microsoft-windows-secconfig-deployment_31bf3856ad364e35_10.0.19041.1_none_179b9b2a4856a91a.manifest"
}

function Get-Windows10UpgradeAssistantPath {
    return Join-Path $env:TEMP "Windows10Upgrade9252.exe"
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

function Test-WslInstalled {
    if (-not (Test-Command "wsl")) {
        return $false
    }

    $previousErrorActionPreference = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $wslOutput = (& wsl --status 2>&1) -join "`n"
    $status = $LASTEXITCODE
    $ErrorActionPreference = $previousErrorActionPreference

    return $status -eq 0 -and $wslOutput -notmatch "is not installed"
}

function Test-DockerResponding {
    try {
        $docker = Get-DockerCli
        & $docker info *> $null
        return $LASTEXITCODE -eq 0
    } catch {
        return $false
    }
}

function Start-DockerDesktopIfInstalled {
    $dockerDesktop = "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    if (Test-Path $dockerDesktop) {
        Write-Host "Starting Docker Desktop..."
        Start-Process -FilePath $dockerDesktop -WindowStyle Hidden
    }
}

function Wait-Docker {
    param([int]$Seconds = 90)

    $deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $deadline) {
        if (Test-DockerResponding) {
            return $true
        }
        Start-Sleep -Seconds 3
    }

    return $false
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

function Register-SetupResumeTask {
    param(
        [Parameter(Mandatory = $true)]
        [string]$InstallMode
    )

    $resumeArgs = @("install", "-ResumeAfterReboot")
    if ($InstallMode -eq "wsl") {
        $resumeArgs += "-UseWsl"
    } elseif ($InstallMode -eq "hyperv") {
        $resumeArgs += "-NoWsl"
    }

    $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument (
        "-NoProfile -ExecutionPolicy Bypass -File `"$PSScriptRoot\icda.ps1`" $($resumeArgs -join ' ')"
    )
    $trigger = New-ScheduledTaskTrigger -AtLogOn -User $env:USERNAME
    $principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -RunLevel Highest -LogonType Interactive
    $task = New-ScheduledTask -Action $action -Trigger $trigger -Principal $principal

    Register-ScheduledTask -TaskName $ResumeTaskName -InputObject $task -Force | Out-Null
}

function Unregister-SetupResumeTask {
    Unregister-ScheduledTask -TaskName $ResumeTaskName -Confirm:$false -ErrorAction SilentlyContinue
}

function Invoke-RequirementCheck {
    param([string]$InstallMode = "auto")

    $wingetOk = Test-Command "winget"
    $gitOk = Test-Command "git"
    $windowsOk = Test-DockerDesktopSupportedWindows
    $dockerCliOk = $false
    try {
        $null = Get-DockerCli
        $dockerCliOk = $true
    } catch {
        $dockerCliOk = $false
    }

    $dockerAllUsersInstall = Test-Path "C:\Program Files\Docker\Docker\Docker Desktop.exe"
    $hyperVEnabled = Test-WindowsFeatureEnabled "Microsoft-Hyper-V-All"
    $containersEnabled = Test-WindowsFeatureEnabled "Containers"
    $wslInstalled = Test-WslInstalled

    Write-Check "winget" $wingetOk "Install App Installer from Microsoft Store."
    Write-Check "git" $gitOk "Run: winget install --exact --id Git.Git"
    Write-Check "Windows 10/11 Docker Desktop support" $windowsOk "Run Windows Update to Windows 10 22H2 build 19045 or newer."

    if ($InstallMode -eq "hyperv") {
        Write-Check "Hyper-V" $hyperVEnabled "Run: scripts\icda.cmd install"
        Write-Check "Windows Containers feature" $containersEnabled "Run: scripts\icda.cmd install"
        Write-Check "Docker Desktop all-users install" $dockerAllUsersInstall "Run: scripts\icda.cmd install"
    } elseif ($InstallMode -eq "wsl") {
        Write-Check "WSL2" $wslInstalled "Run from an elevated PowerShell: wsl --install"
    } else {
        Write-Check "WSL2" $wslInstalled "Auto mode prefers WSL first. Install with: wsl --install"
        Write-Check "Hyper-V" $hyperVEnabled "Fallback path only. Run: scripts\icda.cmd install -NoWsl"
    }

    Write-Check "Docker CLI" $dockerCliOk "Run: scripts\icda.cmd install"

    $dockerReady = $false
    if ($dockerCliOk) {
        $dockerReady = Test-DockerResponding
        Write-Check "Docker engine running" $dockerReady "Start Docker Desktop, then re-run: scripts\icda.cmd ready"
        Invoke-Docker @("--version")
    }

    if ($InstallMode -eq "hyperv") {
        return $wingetOk -and $gitOk -and $windowsOk -and $hyperVEnabled -and $containersEnabled -and $dockerAllUsersInstall -and $dockerCliOk -and $dockerReady
    }

    return $wingetOk -and $gitOk -and $windowsOk -and $wslInstalled -and $dockerCliOk -and $dockerReady
}

function Invoke-WslInstall {
    param(
        [switch]$ResumeMode
    )

    if (-not (Test-Admin)) {
        Write-Host "The WSL setup path needs elevation."
        Start-Process powershell.exe -Verb RunAs -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$PSScriptRoot\icda.ps1`"",
            "install",
            "-UseWsl"
        ) -WorkingDirectory $RepoRoot
        Write-Host ""
        Write-Host "Started elevated WSL setup in a new PowerShell window."
        Write-Host "Accept the Windows UAC prompt. If Windows asks for a reboot, reboot first."
        Write-Host "When Docker Desktop finishes installing, run:"
        Write-Host "  scripts\icda.cmd ready"
        return $false
    }

    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget is required. Install App Installer from Microsoft Store, then re-run scripts\icda.cmd ready."
    }

    Write-Host "Installing Git with winget..."
    winget install --exact --id Git.Git --accept-package-agreements --accept-source-agreements

    Write-Host ""
    if (-not $ResumeMode -and -not (Test-WslInstalled)) {
        Write-Host "Installing WSL..."
        Invoke-CommandChecked -FilePath "wsl.exe" -Arguments @("--install") -FailureMessage "WSL installation failed. Windows reported an error while enabling required components."
        Register-SetupResumeTask -InstallMode "wsl"
        Write-Host ""
        Write-Host "WSL setup needs a reboot to finish."
        Write-Host "I registered an automatic resume task named '$ResumeTaskName'."
        Write-Host "After you log in again, it will continue with Docker Desktop installation."
        Write-Host ""
        Write-Host "Rebooting in 15 seconds. Save anything important now."
        shutdown.exe /r /t 15 /c "ICDA setup enabled WSL and will resume Docker Desktop installation after reboot."
        exit 3010
    }

    Unregister-SetupResumeTask

    if (-not (Test-WslInstalled)) {
        throw "WSL installation did not complete successfully. Reboot the PC, then run scripts\icda.cmd install -UseWsl again."
    }

    if (-not (Test-DockerDesktopSupportedWindows)) {
        Write-Host ""
        Write-Host "WSL is installed and ready."
        Write-Host (Get-DockerDesktopUnsupportedMessage)
        return $true
    }

    Write-Host "Installing Docker Desktop with winget for the WSL2 backend..."
    winget install --exact --id Docker.DockerDesktop --accept-package-agreements --accept-source-agreements

    Write-Host ""
    Write-Host "WSL2 mode is configured. Docker Desktop may still need a restart or first launch."

    Write-Host ""
    Write-Host "After any requested reboot, start Docker Desktop and run:"
    Write-Host "  scripts\icda.cmd ready"
    return $true
}

function Invoke-Repair {
    if (-not (Test-Admin)) {
        Write-Host "The repair path needs elevation."
        Start-Process powershell.exe -Verb RunAs -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$PSScriptRoot\icda.ps1`"",
            "repair"
        ) -WorkingDirectory $RepoRoot
        Write-Host ""
        Write-Host "Started elevated repair in a new PowerShell window."
        Write-Host "Accept the Windows UAC prompt."
        return $false
    }

    $missingManifestPath = Get-SecConfigDeploymentManifestPath
    if (-not (Test-Path $missingManifestPath)) {
        Write-Host "The older SecConfig deployment manifest is still missing:"
        Write-Host "  $missingManifestPath"
        Write-Host "Running component cleanup to remove superseded payload references..."
    } else {
        Write-Host "SecConfig deployment manifest exists; running component cleanup and integrity repair anyway..."
    }

    Invoke-CommandChecked -FilePath "DISM.exe" -Arguments @("/Online", "/Cleanup-Image", "/StartComponentCleanup") -FailureMessage "Component cleanup failed."
    Invoke-CommandChecked -FilePath "DISM.exe" -Arguments @("/Online", "/Cleanup-Image", "/RestoreHealth") -FailureMessage "RestoreHealth failed."
    Invoke-CommandChecked -FilePath "sfc.exe" -Arguments @("/scannow") -FailureMessage "SFC failed."

    Write-Host ""
    Write-Host "Repair pass complete."
    Write-Host "Reboot, then retry:"
    Write-Host "  wsl --install"
    Write-Host "If that succeeds, run:"
    Write-Host "  scripts\icda.cmd ready -UseWsl"
    return $true
}

function Invoke-RepairUpgrade {
    $assistantPath = Get-Windows10UpgradeAssistantPath

    if (-not (Test-Path $assistantPath)) {
        Write-Host "Downloading Microsoft's Windows 10 Update Assistant..."
        Invoke-WebRequest -Uri $Windows10UpgradeAssistantUrl -OutFile $assistantPath
    }

    Write-Host "Launching Microsoft's Windows 10 Update Assistant..."
    Start-Process -FilePath $assistantPath
    Write-Host ""
    Write-Host "Use it to update this PC to Windows 10 22H2."
    Write-Host "That should replace the missing SecConfig/VirtualMachinePlatform payloads that CBS is failing to stage."
    Write-Host "After the upgrade finishes, run:"
    Write-Host "  wsl --install"
    Write-Host "Then run:"
    Write-Host "  scripts\icda.cmd ready -UseWsl"
    return $true
}

function Invoke-HyperVInstall {
    param(
        [switch]$ResumeMode
    )

    Assert-DockerDesktopSupportedWindows

    if (-not (Test-Admin)) {
        Write-Host "The Hyper-V setup path needs elevation."
        Start-Process powershell.exe -Verb RunAs -ArgumentList @(
            "-NoProfile",
            "-ExecutionPolicy", "Bypass",
            "-File", "`"$PSScriptRoot\icda.ps1`"",
            "install",
            "-NoWsl"
        ) -WorkingDirectory $RepoRoot
        Write-Host ""
        Write-Host "Started elevated Hyper-V setup in a new PowerShell window."
        Write-Host "Accept the Windows UAC prompt. If Windows asks for a reboot, reboot first."
        Write-Host "When Docker Desktop finishes installing, run:"
        Write-Host "  scripts\icda.cmd ready -NoWsl"
        return $false
    }

    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
        throw "winget is required. Install App Installer from Microsoft Store, then re-run scripts\icda.cmd ready."
    }

    Write-Host "Installing Git with winget..."
    winget install --exact --id Git.Git --accept-package-agreements --accept-source-agreements

    Write-Host ""
    Write-Host "Enabling Hyper-V Docker Desktop requirements..."
    if ($ResumeMode) {
        $hyperVResult = [pscustomobject]@{ RestartNeeded = $false }
        $containersResult = [pscustomobject]@{ RestartNeeded = $false }
    } else {
        $hyperVResult = Enable-WindowsOptionalFeature -Online -FeatureName Microsoft-Hyper-V-All -All -NoRestart
        $containersResult = Enable-WindowsOptionalFeature -Online -FeatureName Containers -All -NoRestart
    }

    $hyperVReady = Test-WindowsFeatureEnabled "Microsoft-Hyper-V-All"
    $containersReady = Test-WindowsFeatureEnabled "Containers"
    $restartNeeded = $hyperVResult.RestartNeeded -or $containersResult.RestartNeeded

    if ($restartNeeded -or -not ($hyperVReady -and $containersReady)) {
        Register-SetupResumeTask -InstallMode "hyperv"
        Write-Host ""
        Write-Host "Hyper-V/Containers were enabled, but Windows must reboot before Docker can install in Hyper-V mode."
        Write-Host "I registered an automatic resume task named '$ResumeTaskName'."
        Write-Host "After you log in again, it will continue installing Docker Desktop."
        Write-Host ""
        Write-Host "Rebooting in 15 seconds. Save anything important now."
        shutdown.exe /r /t 15 /c "ICDA setup enabled Hyper-V and will resume Docker Desktop installation after reboot."
        exit 3010
    }

    Unregister-SetupResumeTask
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
    Write-Host ""
    Write-Host "After any requested reboot, start Docker Desktop and run:"
    Write-Host "  scripts\icda.cmd ready -NoWsl"
    return $true
}

function Invoke-Install {
    param(
        [string]$InstallMode,
        [switch]$ResumeMode
    )

    if ($InstallMode -eq "wsl") {
        return Invoke-WslInstall -ResumeMode:$ResumeMode
    }

    if ($InstallMode -eq "hyperv") {
        Assert-DockerDesktopSupportedWindows
        return Invoke-HyperVInstall -ResumeMode:$ResumeMode
    }

    try {
        return Invoke-WslInstall -ResumeMode:$ResumeMode
    } catch {
        if ($_.Exception.Message -like "Docker Desktop requires Windows 10 22H2 build 19045 or newer*") {
            throw
        }
        if ($_.Exception.Message -like "Docker Desktop still requires Windows 10 22H2 build 19045 or newer*") {
            throw
        }
        Write-Host ""
        Write-Host "WSL setup path failed. Falling back to Hyper-V."
        Write-Host $_.Exception.Message
        Write-Host ""
        Assert-DockerDesktopSupportedWindows
        return Invoke-HyperVInstall -ResumeMode:$ResumeMode
    }
}

function Invoke-DockerImage {
    Invoke-Docker @("build", "-t", $ImageName, ".")
}

function Invoke-DockerMake {
    param(
        [string]$Target = "",
        [switch]$Interactive
    )

    $ttyArgs = @()
    if ($Interactive -and -not [Console]::IsInputRedirected) {
        $ttyArgs = @("-it")
    }

    $makeArgs = @("make")
    if ($Target) {
        $makeArgs += $Target
    }

    $runArgs = @("run", "--rm") + $ttyArgs + @("-v", "${RepoRoot}:/workspace", "-w", "/workspace", $ImageName) + $makeArgs
    Invoke-Docker $runArgs
}

function Invoke-Ready {
    param([string]$InstallMode, [switch]$NoSmokeMode)

    try {
        $null = Get-DockerCli
        if (-not (Test-DockerResponding)) {
            Start-DockerDesktopIfInstalled
            $null = Wait-Docker
        }
    } catch {
        # The requirement check below will report the missing Docker CLI and
        # trigger the installer path.
    }

    if (-not (Invoke-RequirementCheck -InstallMode $InstallMode)) {
        Write-Host ""
        Write-Host "Installing missing host requirements..."
        $installCompletedHere = Invoke-Install -InstallMode $InstallMode
        if (-not $installCompletedHere) {
            exit 0
        }
        if (-not (Invoke-RequirementCheck -InstallMode $InstallMode)) {
            Write-Host ""
            Write-Host "Docker is not ready yet. If setup requested a reboot or first Docker Desktop launch, do that and run: scripts\icda.cmd ready"
            exit 1
        }
    }

    if (-not (Test-DockerResponding)) {
        Start-DockerDesktopIfInstalled
        if (-not (Wait-Docker)) {
            throw "Docker Desktop is installed but the Docker engine did not become ready. Start Docker Desktop and run scripts\icda.cmd ready again."
        }
    }

    Invoke-DockerImage
    Invoke-DockerMake

    if (-not $NoSmokeMode) {
        Invoke-DockerMake -Target "qemu-smoke"
    }

    Write-Host ""
    Write-Host "ICDA is ready. Built Docker image '$ImageName' and kernel.iso."
}

Push-Location $RepoRoot
try {
    $installMode = Get-InstallMode -RequestedMode $Mode -UseWslMode:$UseWsl -NoWslMode:$NoWsl

    switch ($Action) {
        "ready" { Invoke-Ready -InstallMode $installMode -NoSmokeMode:$NoSmoke }
        "check" {
            if (-not (Invoke-RequirementCheck -InstallMode $installMode)) {
                exit 1
            }
        }
        "install" { [void](Invoke-Install -InstallMode $installMode -ResumeMode:$ResumeAfterReboot) }
        "repair" { [void](Invoke-Repair) }
        "repair-upgrade" { [void](Invoke-RepairUpgrade) }
        "image" { Invoke-DockerImage }
        "build" {
            Invoke-DockerImage
            Invoke-DockerMake
        }
        "smoke" {
            Invoke-DockerImage
            Invoke-DockerMake -Target "qemu-smoke"
        }
        "qemu" {
            Invoke-DockerImage
            $target = "qemu"
            if ($Uefi -and $Headless) {
                $target = "qemu-uefi-headless"
            } elseif ($Uefi) {
                $target = "qemu-uefi"
            } elseif ($Headless) {
                $target = "qemu-headless"
            }
            Invoke-DockerMake -Target $target -Interactive
        }
        "clean" {
            Invoke-DockerImage
            Invoke-DockerMake -Target "clean"
        }
    }
} finally {
    Pop-Location
}
