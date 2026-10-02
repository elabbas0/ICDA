param(
    [string]$OutFile = 'docs/code-comments.md'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$diff = git diff HEAD -- .
$currentFile = ''
$out = New-Object System.Text.StringBuilder
$out.AppendLine('# Moved code comments') | Out-Null
$out.AppendLine('') | Out-Null
$out.AppendLine('This file preserves comments that were removed from the source tree by the comment-stripping task.') | Out-Null
$out.AppendLine('') | Out-Null

foreach ($line in $diff) {
    if ($line.StartsWith('diff --git a/')) {
        $known = $line.Substring(12)
        if ($known.Contains(' b/')) {
            $known = $known.Substring(0, $known.IndexOf(' b/'))
        }
        if ($currentFile -ne '') {
            $out.AppendLine('') | Out-Null
        }
        $out.AppendLine("## $known") | Out-Null
        $out.AppendLine('') | Out-Null
        $currentFile = $known
        continue
    }
    if ($line.StartsWith('-') -and -not $line.StartsWith('---')) {
        $text = $line.Substring(1)
        $isComment = $false
        if ($text -match '//' -or $text -match '/\*' -or $text -match '\*/') {
            $isComment = $true
        }
        if ($text -match '^\s*#' -and ($currentFile -match '\.(py|sh|ps1|pl|cfg|txt|md|yml|yaml)$' -or $currentFile -eq 'Makefile' -or $currentFile -like 'GNUMakefile*')) {
            $isComment = $true
        }
        if ($text -match '^\s*;' -and $currentFile -match '\.(asm|s|S)$') {
            $isComment = $true
        }
        if ($isComment) {
            $out.AppendLine($text) | Out-Null
        }
    }
}

[System.IO.File]::WriteAllText((Join-Path (Get-Location) $OutFile), $out.ToString(), [System.Text.Encoding]::UTF8)
