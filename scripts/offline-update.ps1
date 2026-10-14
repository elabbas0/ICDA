param(
    [Parameter(Mandatory=$true)][string]$Payload,
    [string]$LinuxRoot = '.verify/linuxroot',
    [string]$DriveLetter = 'R',
    [string]$ChatGPTAuthCache = '',
    [switch]$ReadLogsOnly
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$report = Join-Path $repo '.verify/offline-update-result.json'
$icdaGuid = '{5e2a3f8c-1d4b-4e6a-9c7d-1cda00000001}'
$basicGuid = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$mounted = $false
$changedType = $false
$result = @{status='failed';message='';copied=0;verified=0}
function Checked-Child([string]$base, [string]$relative) {
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '(^|[\\/])\.\.([\\/]|$)') {
        throw 'Invalid update path'
    }
    $root = [IO.Path]::GetFullPath($base).TrimEnd('\') + '\'
    $path = [IO.Path]::GetFullPath((Join-Path $root $relative))
    if (!$path.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)) {throw 'Update path escapes its root'}
    return $path
}
try {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if (!([Security.Principal.WindowsPrincipal]$identity).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Windows administrator access is required to mount the ICDA system partition.'
    }
    if ($DriveLetter -notmatch '^[D-Z]$' -or (Test-Path "$($DriveLetter):\")) {throw 'Choose an unused drive letter'}
    $partitions=@(Get-Partition | Where-Object GptType -eq $icdaGuid)
    if ($partitions.Count -ne 1) {throw 'Expected exactly one ICDA partition'}
    $part=$partitions[0]
    if ($part.IsBoot -or $part.IsSystem -or $part.Size -lt 1GB -or $part.Offset -lt 1MB -or $part.AccessPaths) {
        throw 'ICDA partition identity or mount state is unexpected'
    }
    $result.disk=$part.DiskNumber; $result.partition=$part.PartitionNumber
    $payloadRoot=(Resolve-Path -LiteralPath $Payload).ProviderPath
    $linuxSource=(Resolve-Path -LiteralPath $LinuxRoot).ProviderPath
    if (!$ReadLogsOnly) {
        if($ChatGPTAuthCache) {
            $auth=Get-Content -LiteralPath $ChatGPTAuthCache -Raw | ConvertFrom-Json
            if($auth.auth_mode -ne 'chatgpt' -or !$auth.tokens.access_token -or !$auth.tokens.refresh_token) {
                throw 'A valid file-backed ChatGPT Codex sign-in is required'
            }
        }
        $catalog=Get-Content -LiteralPath (Checked-Child $payloadRoot 'offline-files.json') -Raw | ConvertFrom-Json
        if (!$catalog.version -or !$catalog.files.Count) {throw 'Offline update catalog is empty'}
        # Validate every input before mounting or changing any installed file.
        foreach($file in $catalog.files) {
            if ($file.target -notmatch '^(SYSTEM/|linux/|EFI/ICDA/(KERNEL\.BIN|GRUBX64\.EFI)$)' -or
                $file.sourceRoot -notin @('payload','linux')) {throw 'Unexpected offline update target'}
            $source=Checked-Child $(if($file.sourceRoot -eq 'linux'){$linuxSource}else{$payloadRoot}) $file.source
            if (!(Test-Path -LiteralPath $source -PathType Leaf) -or
                (Get-Item -LiteralPath $source).Length -ne $file.size -or
                (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $file.sha256) {
                throw "Offline source verification failed: $($file.target)"
            }
        }
    }
    # The installer is deliberately not used. Windows can mount this FAT32
    # volume after a temporary type change, restored in finally.
    Set-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -GptType $basicGuid
    $changedType=$true
    $mount="$($DriveLetter):\"
    Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AccessPath $mount
    $mounted=$true
    if((Get-Volume -DriveLetter $DriveLetter).FileSystem -ne 'FAT32'){throw 'ICDA system volume is not FAT32'}
    if (!(Test-Path -LiteralPath (Checked-Child $mount 'ICDACFG.TXT')) -or
        !(Test-Path -LiteralPath (Checked-Child $mount 'EFI/ICDA/KERNEL.BIN')) -or
        !(Test-Path -LiteralPath (Checked-Child $mount 'SYSTEM/sbin/init.app'))) {
        throw 'Mounted volume lacks ICDA installation markers'
    }
    foreach($log in @('BOOTLOG.TXT','BOOTLOG.OLD')) {
        $logPath=Checked-Child $mount $log
        if(Test-Path -LiteralPath $logPath){Copy-Item -LiteralPath $logPath -Destination (Join-Path $repo ".verify/installed-$log") -Force}
    }
    if($ReadLogsOnly) { $result.status='logs-read' }
    else {
        $backup=Join-Path $repo ('.verify/offline-backup-' + $catalog.version + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
        New-Item -ItemType Directory -Path $backup | Out-Null
        $result.backup=$backup; $result.version=$catalog.version
        $changed=@()
        foreach($file in $catalog.files) {
            $target=Checked-Child $mount $file.target
            $same=(Test-Path -LiteralPath $target -PathType Leaf) -and
                (Get-Item -LiteralPath $target).Length -eq $file.size -and
                (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -eq $file.sha256
            if($same){$result.verified++;continue}
            if(Test-Path -LiteralPath $target) {
                $saved=Checked-Child $backup $file.target
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved) | Out-Null
                Copy-Item -LiteralPath $target -Destination $saved
            }
            $changed += $file
        }
        $changed | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $backup 'changed-files.json') -Encoding ASCII
        # Write the kernel last. Preserve ICDAROOT.BIN, settings, model/auth
        # files outside this exact catalog, and all boot registration entries.
        foreach($file in @($changed | Sort-Object @{Expression={if($_.target -eq 'EFI/ICDA/KERNEL.BIN'){2}elseif($_.target -eq 'SYSTEM/etc/icda-release.txt'){1}else{0}}})) {
            $target=Checked-Child $mount $file.target
            $source=Checked-Child $(if($file.sourceRoot -eq 'linux'){$linuxSource}else{$payloadRoot}) $file.source
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
            $staged=Checked-Child $mount ($file.target + '.icda-update.tmp')
            Copy-Item -LiteralPath $source -Destination $staged -Force
            if((Get-FileHash -LiteralPath $staged -Algorithm SHA256).Hash -ne $file.sha256){throw "Staged file verification failed: $($file.target)"}
            Move-Item -LiteralPath $staged -Destination $target -Force
            if((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $file.sha256){throw "Installed file verification failed: $($file.target)"}
            $result.copied++; $result.verified++
        }
        $result.status='installed'
        if($ChatGPTAuthCache) {
            $authTarget=Checked-Child $mount 'linux/var/lib/icda/codex/auth.json'
            if(Test-Path -LiteralPath $authTarget){$result.account='existing ICDA sign-in preserved'}
            else {
                New-Item -ItemType Directory -Force -Path (Split-Path -Parent $authTarget) | Out-Null
                Copy-Item -LiteralPath $ChatGPTAuthCache -Destination $authTarget
                if((Get-FileHash -LiteralPath $authTarget -Algorithm SHA256).Hash -ne
                    (Get-FileHash -LiteralPath $ChatGPTAuthCache -Algorithm SHA256).Hash){throw 'Private account-cache copy failed'}
                $result.account='ChatGPT sign-in cache imported privately'
            }
        }
    }
} catch {
    $result.message=$_.Exception.Message
    throw
} finally {
    try {
        if($mounted){Remove-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AccessPath $mount}
    } finally {
        if($changedType){Set-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -GptType $icdaGuid}
        $result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $report -Encoding UTF8
    }
}
Write-Host "ICDA offline update: $($result.status); $($result.verified) files verified."
