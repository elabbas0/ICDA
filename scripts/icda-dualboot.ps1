<#
  ICDA dual-boot installer for a UEFI/GPT Windows PC.

  Run from an elevated PowerShell (Run as administrator):
      powershell -ExecutionPolicy Bypass -File scripts\icda-dualboot.ps1
  Re-run after every build to put the new version on the disk.

  What it does (first run):
    1. Creates one FAT32 partition ("ICDA System") in the disk's unallocated
       space. Existing partitions are never resized, moved or formatted.
    2. Copies the ICDA kernel and system image (ICDAROOT.BIN) onto it, then
       marks it with ICDA's partition type so Windows leaves it alone.
    3. Copies GRUB to \EFI\ICDA\GRUBX64.EFI on the EFI system partition.
       Nothing else on the EFI partition is touched (not \EFI\Microsoft,
       not \EFI\Boot).
    4. Backs up the boot configuration, then adds an "ICDA" UEFI boot entry
       at the END of the boot order. Windows stays the default; pick ICDA
       from the firmware boot menu (F12 at the Dell logo). GRUB's menu
       also offers Windows.
  Later runs update the files in place.

  Options:
    -RootSizeGB n   size of the new partition on first install (default 4)
    -KeepData       keep the existing ICDAROOT.BIN (your files in ICDA);
                    only kernel and GRUB are updated
    -DryRun         show what would happen, change nothing
    -Uninstall      remove the boot entry, \EFI\ICDA and the ICDA partition
#>
param(
    [string]$Deploy = "",
    [int]$RootSizeGB = 4,
    [switch]$KeepData,
    [switch]$DryRun,
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$IcdaGuid  = "{5e2a3f8c-1d4b-4e6a-9c7d-1cda00000001}"
$BasicGuid = "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}"
$EspGuid   = "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}"
$EntryName = "ICDA"

if (-not $Deploy) { $Deploy = Join-Path (Split-Path -Parent $PSScriptRoot) ".verify\deploy" }

function Say($msg)  { Write-Host $msg }
function Step($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Fail($msg) { Write-Host ""; Write-Host "STOPPED: $msg" -ForegroundColor Red; exit 1 }

# ---- preconditions -------------------------------------------------------

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { Fail "run this from an elevated PowerShell (right-click PowerShell > Run as administrator)." }

if ($env:firmware_type -ne "UEFI") { Fail "this PC is not booted in UEFI mode." }

$disk = Get-Disk | Where-Object { $_.IsBoot -and $_.IsSystem } | Select-Object -First 1
if (-not $disk) { Fail "could not find the Windows boot disk." }
if ($disk.PartitionStyle -ne "GPT") { Fail "the boot disk is not GPT." }
$parts = Get-Partition -DiskNumber $disk.Number | Sort-Object Offset
$esp  = $parts | Where-Object { $_.GptType -eq $EspGuid } | Select-Object -First 1
if (-not $esp) { Fail "no EFI system partition found on disk $($disk.Number)." }
$icda = $parts | Where-Object { $_.GptType -eq $IcdaGuid } | Select-Object -First 1

Say ("Disk {0}: {1}, {2:N1} GB, GPT" -f $disk.Number, $disk.FriendlyName, ($disk.Size / 1GB))
foreach ($p in $parts) {
    Say ("  #{0}  {1,-10} {2,8:N2} GB at {3,8:N2} GB  {4}" -f $p.PartitionNumber, $p.Type, ($p.Size / 1GB), ($p.Offset / 1GB), $p.DriveLetter)
}

# Secure Boot blocks unsigned GRUB; BitLocker may ask for its recovery key
# after boot entries change.  Report both, change neither.
$secureBoot = $null
try { $secureBoot = Confirm-SecureBootUEFI } catch { }
$bitlocker = $null
try { $bitlocker = (Get-BitLockerVolume -MountPoint "C:" -ErrorAction Stop).ProtectionStatus } catch { }

# ---- helpers ---------------------------------------------------------------

function Free-Letter {
    $used = (Get-Volume | Where-Object DriveLetter | ForEach-Object { [string]$_.DriveLetter }) + `
            (Get-PSDrive -PSProvider FileSystem | ForEach-Object { $_.Name })
    foreach ($c in [char[]]"RSTUVWXYQPON") { if ($used -notcontains [string]$c) { return [string]$c } }
    Fail "no free drive letter."
}

function Set-PartType($part, $guid) {
    # Set-Partition -GptType where available; diskpart otherwise.
    try {
        Set-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -GptType $guid -ErrorAction Stop
    } catch {
        $g = $guid.Trim('{', '}')
        $script = "select disk $($part.DiskNumber)`r`nselect partition $($part.PartitionNumber)`r`nset id=$g override`r`n"
        $tmp = [IO.Path]::GetTempFileName()
        Set-Content -Path $tmp -Value $script -Encoding ASCII
        diskpart /s $tmp | Out-Null
        Remove-Item $tmp
    }
}

function Mount-IcdaPartition($part) {
    Set-PartType $part $BasicGuid
    Start-Sleep -Milliseconds 800
    # Windows may have mounted it on its own once it looks like a data partition
    $now = Get-Partition -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber
    if ($now.DriveLetter) { return [string]$now.DriveLetter }
    $letter = Free-Letter
    Add-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AccessPath "$($letter):\"
    Start-Sleep -Milliseconds 800
    return $letter
}

function Unmount-IcdaPartition($part, $letter) {
    Remove-PartitionAccessPath -DiskNumber $part.DiskNumber -PartitionNumber $part.PartitionNumber -AccessPath "$($letter):\"
    Set-PartType $part $IcdaGuid
}

function Find-BootEntry {
    $out = bcdedit /enum firmware
    $id = $null
    foreach ($line in $out) {
        if ($line -match '^identifier\s+(\{[0-9a-fA-F-]+\})') { $id = $Matches[1] }
        if ($line -match '^description\s+(.+)$' -and $Matches[1].Trim() -eq $EntryName) { return $id }
    }
    return $null
}

# ---- uninstall -------------------------------------------------------------

if ($Uninstall) {
    Step "Uninstall plan"
    Say "  remove UEFI boot entry '$EntryName', delete \EFI\ICDA on the EFI partition, delete the ICDA partition"
    if ($DryRun) { exit 0 }
    if ((Read-Host "Type UNINSTALL to continue") -ne "UNINSTALL") { Fail "cancelled." }
    $entry = Find-BootEntry
    if ($entry) { bcdedit /delete $entry | Out-Null; Say "  boot entry removed" }
    $l = Free-Letter
    mountvol "$($l):" /S
    try { if (Test-Path "$($l):\EFI\ICDA") { Remove-Item -Recurse -Force "$($l):\EFI\ICDA" }; Say "  \EFI\ICDA removed" }
    finally { mountvol "$($l):" /D }
    if ($icda) { Remove-Partition -DiskNumber $icda.DiskNumber -PartitionNumber $icda.PartitionNumber -Confirm:$false; Say "  ICDA partition deleted (space is unallocated again)" }
    Say ""; Say "ICDA removed."
    exit 0
}

# ---- install plan ------------------------------------------------------------

foreach ($f in "GRUBX64.EFI", "KERNEL.BIN", "ICDAROOT.BIN", "ICDACFG.TXT") {
    if (-not (Test-Path (Join-Path $Deploy $f))) { Fail "missing $Deploy\$f (build the deploy files first)." }
}
$grubSize = (Get-Item (Join-Path $Deploy "GRUBX64.EFI")).Length

$newOffset = $null
$newSize = [int64]$RootSizeGB * 1GB
if (-not $icda) {
    # largest unallocated gap between existing partitions, 1 MiB aligned
    $best = $null
    $cursor = [int64]1MB
    $edges = @($parts | ForEach-Object { [pscustomobject]@{ Start = [int64]$_.Offset; End = [int64]($_.Offset + $_.Size) } })
    $edges += [pscustomobject]@{ Start = [int64]($disk.Size - 1MB); End = [int64]$disk.Size }
    foreach ($e in $edges) {
        $start = [int64]([math]::Ceiling($cursor / 1MB) * 1MB)
        $gap = $e.Start - $start
        if ($gap -gt 0 -and (-not $best -or $gap -gt $best.Size)) { $best = [pscustomobject]@{ Offset = $start; Size = $gap } }
        if ($e.End -gt $cursor) { $cursor = $e.End }
    }
    if (-not $best -or $best.Size -lt $newSize) { Fail ("not enough unallocated space for a {0} GB partition." -f $RootSizeGB) }
    $newOffset = $best.Offset
}

Step "Plan"
if ($icda) {
    Say ("  update existing ICDA partition #{0} ({1:N2} GB)" -f $icda.PartitionNumber, ($icda.Size / 1GB))
    if ($KeepData) { Say "  keep ICDAROOT.BIN (your ICDA files)" } else { Say "  replace ICDAROOT.BIN with the new system image (files saved inside ICDA are reset)" }
} else {
    Say ("  create a {0} GB FAT32 partition at {1:N2} GB, inside unallocated space" -f $RootSizeGB, ($newOffset / 1GB))
}
Say "  copy KERNEL.BIN + ICDAROOT.BIN + ICDACFG.TXT to the ICDA partition"
Say ("  copy GRUBX64.EFI ({0:N1} MB) to \EFI\ICDA on the EFI partition" -f ($grubSize / 1MB))
$entry = Find-BootEntry
if ($entry) { Say "  UEFI boot entry '$EntryName' already exists ($entry)" }
else { Say "  back up the boot configuration, add UEFI boot entry '$EntryName' last in the boot order" }
Say ""
if ($secureBoot -eq $true) {
    Write-Host "  NOTE: Secure Boot is ON. The firmware will refuse to start ICDA's GRUB until you turn" -ForegroundColor Yellow
    Write-Host "        Secure Boot off in the BIOS setup (F2). Windows is not affected either way." -ForegroundColor Yellow
}
if ($bitlocker -eq "On") {
    Write-Host "  NOTE: BitLocker protects C:. Changing boot entries can make Windows ask for the" -ForegroundColor Yellow
    Write-Host "        BitLocker recovery key once. Have it ready (account.microsoft.com/devices/recoverykey)." -ForegroundColor Yellow
}
if ($DryRun) { Say ""; Say "Dry run: nothing changed."; exit 0 }
if ((Read-Host "Type INSTALL to continue") -ne "INSTALL") { Fail "cancelled." }

# ---- 1. ICDA partition -------------------------------------------------------

Step "ICDA partition"
if (-not $icda) {
    $icda = New-Partition -DiskNumber $disk.Number -Offset $newOffset -Size $newSize -GptType $BasicGuid
    Start-Sleep -Seconds 1
    Format-Volume -Partition $icda -FileSystem FAT32 -NewFileSystemLabel "ICDAROOT" -Confirm:$false | Out-Null
    Say ("  created partition #{0}" -f $icda.PartitionNumber)
    $icda = Get-Partition -DiskNumber $disk.Number -PartitionNumber $icda.PartitionNumber
    # Format may have assigned a letter; normalise to our own mount handling
    if ($icda.DriveLetter) {
        Remove-PartitionAccessPath -DiskNumber $icda.DiskNumber -PartitionNumber $icda.PartitionNumber -AccessPath "$($icda.DriveLetter):\"
    }
}
$letter = Mount-IcdaPartition $icda
try {
    $root = "$($letter):\"
    New-Item -ItemType Directory -Force -Path "$root\EFI\ICDA" | Out-Null
    Copy-Item -Force (Join-Path $Deploy "KERNEL.BIN") "$root\EFI\ICDA\KERNEL.BIN"
    if (-not ($KeepData -and (Test-Path "$root\ICDAROOT.BIN"))) {
        Copy-Item -Force (Join-Path $Deploy "ICDAROOT.BIN") "$root\ICDAROOT.BIN"
        Copy-Item -Force (Join-Path $Deploy "ICDACFG.TXT") "$root\ICDACFG.TXT"
    }
    Write-VolumeCache -DriveLetter $letter
    Say "  kernel and system image written"
} finally {
    Unmount-IcdaPartition $icda $letter
}
Say "  partition marked as ICDA System (hidden from Windows)"

# ---- 2. GRUB on the EFI partition --------------------------------------------

Step "EFI system partition"
$el = Free-Letter
mountvol "$($el):" /S
try {
    $drive = New-Object IO.DriveInfo "$($el):"
    $existing = 0
    if (Test-Path "$($el):\EFI\ICDA") { $existing = (Get-ChildItem "$($el):\EFI\ICDA" -Recurse -File | Measure-Object Length -Sum).Sum }
    if ($drive.AvailableFreeSpace + $existing -lt $grubSize + 1MB) {
        Fail ("the EFI partition has only {0:N1} MB free." -f ($drive.AvailableFreeSpace / 1MB))
    }
    New-Item -ItemType Directory -Force -Path "$($el):\EFI\ICDA" | Out-Null
    Copy-Item -Force (Join-Path $Deploy "GRUBX64.EFI") "$($el):\EFI\ICDA\GRUBX64.EFI"
    # a kernel copy here would shadow the one on the ICDA partition
    if (Test-Path "$($el):\EFI\ICDA\KERNEL.BIN") { Remove-Item -Force "$($el):\EFI\ICDA\KERNEL.BIN" }
    Say "  \EFI\ICDA\GRUBX64.EFI written"
} finally {
    mountvol "$($el):" /D
}

# ---- 3. boot entry -------------------------------------------------------------

Step "Boot entry"
$entry = Find-BootEntry
if (-not $entry) {
    $backup = Join-Path $Deploy ("bcd-backup-{0:yyyyMMdd-HHmmss}.bcd" -f (Get-Date))
    bcdedit /export $backup | Out-Null
    Say "  boot configuration backed up to $backup"
    $out = bcdedit /copy "{bootmgr}" /d $EntryName
    if (-not ($out -match '(\{[0-9a-fA-F-]+\})')) { Fail "bcdedit /copy failed: $out" }
    $entry = $Matches[1]
    bcdedit /set $entry path "\EFI\ICDA\GRUBX64.EFI" | Out-Null
    bcdedit /set "{fwbootmgr}" displayorder $entry /addlast | Out-Null
    Say "  added UEFI boot entry '$EntryName' $entry (last; Windows stays first)"
} else {
    Say "  boot entry '$EntryName' already present ($entry)"
}

Step "Done"
Say "  Restart, press F12 at the Dell logo and choose '$EntryName'."
Say "  GRUB shows ICDA and Windows Boot Manager; ICDA starts after 5 seconds."
Say "  To remove ICDA later: run this script with -Uninstall."
