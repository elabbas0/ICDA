param([string]$LinuxRoot = '.verify/linuxroot', [string]$Output = '')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
$header=Get-Content -LiteralPath (Join-Path $repo 'version.h') -Raw
$version=[regex]::Match($header,'ICDA_VERSION_STRING\s+"([0-9.]+)"').Groups[1].Value
if(!$version){throw 'ICDA version is missing'}
if(!$Output){$Output=Join-Path $repo ".verify/offline-$version"}
$payload=[IO.Path]::GetFullPath($Output)
$allowed=[IO.Path]::GetFullPath((Join-Path $repo '.verify')).TrimEnd('\')+'\'
if(!$payload.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Offline payload must stay under .verify'}
New-Item -ItemType Directory -Force -Path $payload | Out-Null
$linux=(Resolve-Path -LiteralPath $LinuxRoot).ProviderPath
$files=New-Object 'System.Collections.Generic.List[object]'
$lines=New-Object 'System.Collections.Generic.List[string]'
$lines.Add('icda-ota 1'); $lines.Add("version $version")
$lines.Add('notes Offline update: terminal-only Codex, Assistant-owned verified Qwen downloads, and terminal and storage fixes.')
function Add-Source([string]$rootKind,[string]$relative,[string]$target) {
    $base=if($rootKind -eq 'linux'){$linux}else{$payload}
    $path=Join-Path $base $relative
    $size=(Get-Item -LiteralPath $path).Length
    $sha=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower()
    $files.Add(@{sourceRoot=$rootKind;source=$relative;target=$target;size=$size;sha256=$sha})
    # Model weights are still described with direct HTTP source rows below.
    if($target -notmatch '^linux/usr/share/icda/models/.*\.(gguf|bin)$') {
        $type=if($rootKind -eq 'linux'){'lfile'}else{'file'}
        $lines.Add("$type $target $size $sha")
    }
}
$core=@(
    @('EFI/ICDA/KERNEL.BIN','kernel/install-kernel.bin'),
    @('EFI/ICDA/GRUBX64.EFI','kernel/fs/bootx64-install.efi')
)
foreach($line in Get-Content -LiteralPath (Join-Path $repo 'boot/system-files.txt')) {
    if($line -match '^\s*([^#\s]\S*)\s+(\S+)\s*$'){$core+=,@("SYSTEM/$($Matches[1])",$Matches[2])}
}
foreach($pair in $core){
    $dest=Join-Path $payload $pair[0]
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dest) | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $pair[1]) -Destination $dest -Force
    Add-Source 'payload' $pair[0] $pair[0]
}
Get-ChildItem -LiteralPath $linux -File -Recurse | Sort-Object FullName | ForEach-Object {
    $relative=$_.FullName.Substring($linux.TrimEnd('\').Length+1).Replace('\','/')
    # Keep the signed model catalog and pinned HTTP sources; do not bundle all
    # model weights in an OS update. The model picker downloads a chosen model.
    if($relative -match '^usr/share/icda/models/.*\.(gguf|bin)$') { return }
    if($relative -match '(^|/)(auth\.json|\.codex)(/|$)' -or $relative -match '^(home|root)/') {
        throw 'Private state must not enter an offline runtime bundle'
    }
    Add-Source 'linux' $relative "linux/$relative"
}
foreach($model in (Get-Content -LiteralPath (Join-Path $linux 'usr/share/icda/models/sources.json') -Raw | ConvertFrom-Json)) {
    $lines.Add("model linux/usr/share/icda/models/$($model.Name) $($model.Size) $($model.Sha) https://huggingface.co/$($model.Repo)/resolve/$($model.Revision)/$($model.Name)")
}
if($files.Count -gt 4096){throw 'Offline manifest exceeds the updater file limit'}
$body=Join-Path $payload 'manifest.body'
[IO.File]::WriteAllText($body,($lines -join "`n")+"`n",[Text.Encoding]::ASCII)
$signDir=Join-Path $env:USERPROFILE '.icda'
if(!(Test-Path -LiteralPath (Join-Path $signDir 'ota-ed25519.pem'))){throw 'ICDA update signing key is missing'}
$repoMount=$repo.Replace('\','/')+':/workspace'
$signMount=$signDir.Replace('\','/')+':/signkey:ro'
$relPayload=$payload.Substring($repo.Length+1).Replace('\','/')
& docker run --rm -v $repoMount -v $signMount -w /workspace icda-toolchain openssl pkeyutl -sign -inkey /signkey/ota-ed25519.pem -rawin -in "$relPayload/manifest.body" -out "$relPayload/manifest.sig"
if($LASTEXITCODE){throw 'Offline manifest signing failed'}
$signature=[BitConverter]::ToString([IO.File]::ReadAllBytes((Join-Path $payload 'manifest.sig'))).Replace('-','').ToLower()
if($signature.Length -ne 128){throw 'Invalid Ed25519 signature length'}
$manifest=($lines -join "`n")+"`nsignature $signature`n"
[IO.File]::WriteAllText((Join-Path $payload 'manifest.txt'),$manifest,[Text.Encoding]::ASCII)
$installed=Join-Path $payload 'SYSTEM/etc/icda-release.txt'
[IO.File]::WriteAllText($installed,$manifest,[Text.Encoding]::ASCII)
$releaseFile=@{sourceRoot='payload';source='SYSTEM/etc/icda-release.txt';target='SYSTEM/etc/icda-release.txt';size=(Get-Item -LiteralPath $installed).Length;sha256=(Get-FileHash -LiteralPath $installed -Algorithm SHA256).Hash.ToLower()}
$files.Add($releaseFile)
@{version=$version;linuxRoot=$linux;files=@($files.ToArray())} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $payload 'offline-files.json') -Encoding ASCII
Write-Host "Prepared signed ICDA $version offline update: $($files.Count) files; $payload"
