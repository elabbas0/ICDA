param([string]$LinuxRoot = '.verify/linuxroot')
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$source = Get-Content -LiteralPath (Join-Path $repo 'resources/codex/source.json') -Raw | ConvertFrom-Json
$cache = Join-Path $repo '.verify/codex'
New-Item -ItemType Directory -Force -Path $cache | Out-Null
$archive = Join-Path $cache 'codex-package.tar.gz'
$package = Join-Path $cache 'package'
if ($source.layout -ne 'package' -or !$source.files.Count) { throw 'Codex package metadata is missing' }
function Test-PackageFiles {
    foreach ($file in $source.files) {
        if ($file.path -notmatch '^[A-Za-z0-9._/-]+$' -or $file.path -match '(^|/)\.\.?(/|$)' -or
            $file.path -notmatch '^(bin/|codex-package\.json$|codex-path/|codex-resources/)') {
            throw 'Invalid package file path'
        }
        $path = Join-Path $package $file.path
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return $false }
        $item = Get-Item -LiteralPath $path
        if ($item.Length -ne $file.size -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -or
            (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $file.sha256) { return $false }
    }
    return $true
}
if (!(Test-PackageFiles)) {
    if (!(Test-Path -LiteralPath $archive) -or
        (Get-Item -LiteralPath $archive).Length -ne $source.archiveSize -or
        (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash -ne $source.archiveSha256) {
        & curl.exe --fail --location --retry 3 --output "$archive.part" $source.url
        if ($LASTEXITCODE) { throw 'Official Codex package download failed' }
        if ((Get-Item -LiteralPath "$archive.part").Length -ne $source.archiveSize -or
            (Get-FileHash -LiteralPath "$archive.part" -Algorithm SHA256).Hash -ne $source.archiveSha256) {
            throw 'Codex package SHA-256 mismatch'
        }
        Move-Item -LiteralPath "$archive.part" -Destination $archive -Force
    }
    $members = @(& tar.exe -tf $archive)
    if ($LASTEXITCODE) { throw 'Could not inspect Codex package' }
    foreach ($member in $members) {
        if ($member -notmatch '^[A-Za-z0-9._/-]+$' -or $member -match '(^|/)\.\.?(/|$)' -or
            $member -notmatch '^(bin/|codex-package\.json$|codex-path/|codex-resources/)') {
            throw 'Codex archive contains an unexpected path'
        }
    }
    New-Item -ItemType Directory -Force -Path $package | Out-Null
    & tar.exe -xf $archive -C $package
    if ($LASTEXITCODE -or !(Test-PackageFiles)) { throw 'Codex package extraction verification failed' }
}
$metadata = Get-Content -LiteralPath (Join-Path $package 'codex-package.json') -Raw | ConvertFrom-Json
if ($metadata.version -ne $source.version -or $metadata.target -ne 'x86_64-unknown-linux-musl' -or
    $metadata.layoutVersion -ne 1 -or $metadata.entrypoint -ne 'bin/codex') {
    throw 'Codex package layout does not match the pinned release'
}
# Keep the official package's bin/metadata/path/resources relationship intact.
# /usr/bin/codex remains the public entry point on ICDA.
$usr = [IO.Path]::GetFullPath((Join-Path $LinuxRoot 'usr'))
foreach ($file in $source.files) {
    $target = [IO.Path]::GetFullPath((Join-Path $usr $file.path))
    if (!$target.StartsWith($usr.TrimEnd('\')+'\', [StringComparison]::OrdinalIgnoreCase)) {
        throw 'Codex destination escaped the Linux root'
    }
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
    Copy-Item -LiteralPath (Join-Path $package $file.path) -Destination $target -Force
}
$context = Join-Path $LinuxRoot 'usr/share/icda/codex'
New-Item -ItemType Directory -Force -Path $context | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repo 'resources/codex') -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $context $_.Name) -Force
}
Write-Host "Verified official Codex $($source.version) package ($($source.files.Count) files), prepared inside $LinuxRoot."
