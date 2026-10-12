param(
    [string]$Text = "",
    [int]$Port = 4444,
    [int]$DelayMs = 40,
    [switch]$Enter
)
# Types a line into the guest through QMP, one QCODE per character.
#   .\scripts\qmp-type.ps1 -Text "ls -l /volumes/ntfs-0" -Enter
$ErrorActionPreference = "Stop"

# The QEMU qcode names that differ from the character.  Letters, digits and the
# rest of the plain ASCII range are their own qcode.
$map = @{
    ' ' = 'spc';  '/' = 'slash';  '.' = 'dot';    '-' = 'minus'; '_' = 'minus'
    ':' = 'semicolon'; ';' = 'semicolon'; ',' = 'comma'; '=' = 'equal'
    '+' = 'shift'; '*' = 'shift'; '>' = 'shift'; '<' = 'shift'
    '#' = 'shift'; '?' = 'shift'; '|' = 'shift'; '~' = 'shift'
    '(' = 'shift'; ')' = 'shift'; '$' = 'shift'
}

function Send-Qmp($obj) {
    $c = New-Object System.Net.Sockets.TcpClient
    $c.Connect("127.0.0.1", $Port)
    $s = $c.GetStream()
    $w = New-Object System.IO.StreamWriter($s)
    $w.AutoFlush = $true
    $r = New-Object System.IO.StreamReader($s)
    $null = $r.ReadLine()                      # greeting
    $w.WriteLine(($obj | ConvertTo-Json -Compress -Depth 10))
    $null = $r.ReadLine()                      # return
    $c.Close()
}

foreach ($ch in $Text.ToCharArray()) {
    $k = $ch.ToString()
    if ($map.ContainsKey($k)) { $k = $map[$k] }
    elseif ($k -match '[a-z]') { $k = $ch.ToString() }
    elseif ($k -match '[A-Z]') { Send-Qmp @{ execute = "qmp_capabilities" } | Out-Null }
    if ($k -match '^[a-z0-9]$') {
        Send-Qmp @{ execute = "send-key"; arguments = @{ keys = @( @{ type = "qcode"; data = $k } ) } }
    } else {
        # anything else needs a modifier: qcode "shift-<key>"
        if ($k.Length -eq 1 -and $k -match '[A-Z0-9!#$%&()*+:<>?@\[\]^{|}~"]') {
            $base = $k.ToLower()
            $q = switch ($base) {
                '!' { '1' } '#' { '3' } '$' { '4' } '%' { '5' } '&' { '7' }
                '(' { '9' } ')' { '0' } '*' { '8' } ':' { 'semicolon' } ';' { 'semicolon' }
                '<' { 'comma' } '>' { 'dot' } '?' { 'slash' } '@' { '2' } '[' { 'bracket_left' }
                ']' { 'bracket_right' } '^' { '6' } '{' { 'bracket_left' } '|' { 'backslash' }
                '}' { 'bracket_right' } '~' { 'grave_accent' } '"' { 'apostrophe' }
                '=' { 'equal' } '+' { 'equal' } default { $base }
            }
            Send-Qmp @{ execute = "send-key"; arguments = @{ keys = @(
                @{ type = "qcode"; data = "shift" }, @{ type = "qcode"; data = $q } ) } }
        }
    }
    Start-Sleep -Milliseconds $DelayMs
}

if ($Enter) {
    Send-Qmp @{ execute = "send-key"; arguments = @{ keys = @( @{ type = "qcode"; data = "ret" } ) } }
}