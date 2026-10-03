$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$ImageName = "icda-toolchain"
$OutFile = Join-Path $RepoRoot "compile_commands.json"

function Get-MakeDryRun {
    param([string[]]$MakeArgs)
    $out = & docker run --rm -v "${RepoRoot}:/workspace" -w /workspace $ImageName make -nB @MakeArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "docker make -nB $($MakeArgs -join ' ') failed. Build the image first: scripts\icda.cmd image"
    }
    return $out
}

function Add-CompileEntry {
    param(
        [System.Collections.Specialized.OrderedDictionary]$ByFile,
        [string]$DirJson,
        [string]$Command
    )
    $t = $Command.Trim()
    if ($t -notmatch '^gcc\s.*\s-c\s+(\S+\.c)\s+-o\s+(\S+)') { return }
    $src = $Matches[1]
    $key = "$DirJson/$src"
    if ($ByFile.Contains($key)) { return }
    $args = @()
    foreach ($tok in ($t -split '\s+')) {
        if ($tok -eq '') { continue }
        $args += $tok
    }
    $ByFile[$key] = [pscustomobject]@{
        directory = $DirJson
        file      = $key
        arguments = $args
    }
}

Push-Location $RepoRoot
try {
    Write-Host "Capturing exact compile commands (docker make -nB)..."
    $dirJson = $RepoRoot -replace '\\', '/'
    $byFile = [ordered]@{}

    foreach ($line in (Get-MakeDryRun @())) { Add-CompileEntry $byFile $dirJson $line }
    foreach ($line in (Get-MakeDryRun @("CI_SELFTEST=1", "CI_IMAGE=1"))) { Add-CompileEntry $byFile $dirJson $line }
    foreach ($line in (Get-MakeDryRun @("sb16.o"))) { Add-CompileEntry $byFile $dirJson $line }

    
    
    $template = @($byFile.Values | Where-Object { $_.file -match '/userspace/' } | Select-Object -First 1)
    if ($template.Count -gt 0) {
        $csrcs = Get-ChildItem -Recurse -Filter *.c -Path (Join-Path $RepoRoot "userspace") |
            ForEach-Object { ($_.FullName -replace '\\', '/') }
        foreach ($abs in $csrcs) {
            if ($byFile.Contains($abs)) { continue }
            $rel = $abs.Substring($dirJson.Length + 1)
            $base = [System.IO.Path]::GetFileNameWithoutExtension($abs)
            $newArgs = @()
            foreach ($tok in $template[0].arguments) {
                if ($tok -eq '-c') { $newArgs += '-c'; continue }
                if ($tok -like 'userspace/*.c') { $newArgs += $rel; continue }
                if ($tok -like '/tmp/icda-*.o') { $newArgs += "/tmp/icda-$base.o"; continue }
                $newArgs += $tok
            }
            
            if ($newArgs -notcontains $rel) { continue }
            $byFile[$abs] = [pscustomobject]@{
                directory = $dirJson
                file      = $abs
                arguments = $newArgs
            }
            Write-Host "  (no Makefile rule, userspace flags assumed) $rel"
        }
    }

    if ($byFile.Count -eq 0) {
        throw "No compile commands parsed. Is the Docker image '$ImageName' up to date?"
    }

    $json = @($byFile.Values) | ConvertTo-Json -Depth 10
    
    [System.IO.File]::WriteAllText($OutFile, $json + [Environment]::NewLine,
        (New-Object System.Text.UTF8Encoding $false))
    Write-Host "Wrote $OutFile with $($byFile.Count) translation units."
} finally {
    Pop-Location
}
