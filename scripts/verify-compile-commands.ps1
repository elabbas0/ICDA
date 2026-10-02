$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$db = Get-Content (Join-Path $RepoRoot "compile_commands.json") -Raw | ConvertFrom-Json
$files = $db.file
$missing = @()
$all = @(Get-ChildItem -Recurse -Filter *.c -Path (Join-Path $RepoRoot "kernel")) +
       @(Get-ChildItem -Recurse -Filter *.c -Path (Join-Path $RepoRoot "userspace"))
foreach ($f in $all) {
    $p = $f.FullName.Replace('\', '/')
    if ($files -notcontains $p) { $missing += $p }
}
foreach ($m in $missing) { Write-Output "MISSING: $m" }
Write-Output ("missing=" + $missing.Count + " total=" + $db.Count + " sources=" + $all.Count)
