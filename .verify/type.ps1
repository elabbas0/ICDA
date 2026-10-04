param([string]$Text, [switch]$Enter, [int]$SettleMs = 80)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$q = "scripts\qmp-input.ps1"
foreach ($ch in $Text.ToCharArray()) {
    $k = switch -CaseSensitive ($ch) {
        '/' { "slash" } '-' { "minus" } '.' { "dot" } ' ' { "spc" } '_' { "shift+minus" } ':' { "shift+semicolon" }
        default { if ($ch -cmatch '[A-Z]') { "shift+" + "$ch".ToLower() } else { "$ch" } }
    }
    powershell -ExecutionPolicy Bypass -File $q -Key $k -SettleMs $SettleMs | Out-Null
}
if ($Enter) { powershell -ExecutionPolicy Bypass -File $q -Key "ret" -SettleMs 600 | Out-Null }
