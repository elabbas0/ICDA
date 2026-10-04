param([string]$Text, [switch]$Enter, [int]$SettleMs = 80)
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$q = "scripts\qmp-input.ps1"
$Text = $Text.Replace("~Q", [string][char]34)
foreach ($ch in $Text.ToCharArray()) {
    $k = switch -CaseSensitive ($ch) {
        '/' { "slash" } '-' { "minus" } '.' { "dot" } ' ' { "spc" } '_' { "shift+minus" } ':' { "shift+semicolon" } '"' { "shift+apostrophe" }
        '|' { "shift+backslash" } '>' { "shift+dot" } '<' { "shift+comma" } ';' { "semicolon" } '&' { "shift+7" }
        '$' { "shift+4" } '=' { "equal" } '*' { "shift+8" } "'" { "apostrophe" } '(' { "shift+9" } ')' { "shift+0" }
        ',' { "comma" } '!' { "shift+1" } '+' { "shift+equal" }
        '?' { "shift+slash" } '[' { "bracket_left" } ']' { "bracket_right" } '{' { "shift+bracket_left" } '}' { "shift+bracket_right" }
        '#' { "shift+3" } '%' { "shift+5" } '~' { "shift+grave_accent" } '\' { "backslash" }
        default { if ($ch -cmatch '[A-Z]') { "shift+" + "$ch".ToLower() } else { "$ch" } }
    }
    powershell -ExecutionPolicy Bypass -File $q -Key $k -SettleMs $SettleMs | Out-Null
}
if ($Enter) { powershell -ExecutionPolicy Bypass -File $q -Key "ret" -SettleMs 600 | Out-Null }
