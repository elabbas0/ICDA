<
.SYNOPSIS
  Regenerate compile_commands.json (clangd LSP database) from the Makefile.

.DESCRIPTION
  Uses the Docker toolchain image as ground truth: `make -nB` prints the
  exact gcc command for every translation unit (kernel CFLAGS vs userspace
  USR_CFLAGS, generated asset sources, per-file -D overrides), and this
  script converts those into compile_commands.json with host paths so any
  clangd client (VS Code, Vim, Emacs, Sublime) gets accurate browse,
  diagnostics, and rename for the tree.

  Three dry-runs are merged so every C source is covered, even ones the
  default product build skips:
    - default build (product image)
    - CI_SELFTEST=1 CI_IMAGE=1 (gui_demo, nptest, nptestlx test apps)
    - the orphan sb16.o rule (compiled but not linked into kernel.bin)
  Sources with no Makefile rule at all (currently audiod.c, mkfiles.c)
  reuse the userspace compile flags of an already-parsed entry.

  Run from the repo root after the Docker image exists:

    powershell -ExecutionPolicy Bypass -File scripts\gen-compile-commands.ps1

  Requires: Docker engine running, image icda-toolchain (scripts\icda.cmd
  image builds it). No Python / bear / compiledb needed on the host.


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
