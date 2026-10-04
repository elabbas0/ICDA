param([int]$Frames = 60, [int]$IntervalMs = 80, [string]$Prefix = ".verify/burst")
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$client = $null
for ($try = 0; $try -lt 50 -and -not $client; $try++) {
    try { $client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4444) } catch { $client = $null; Start-Sleep -Milliseconds 100 }
}
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream)
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
$writer.NewLine = "`n"
[void]$reader.ReadLine()
$id = 0
function Cmd($obj) {
    $script:id++
    $obj["id"] = $script:id
    $writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 10))
    while ($true) {
        $msg = $reader.ReadLine() | ConvertFrom-Json
        if ($msg.PSObject.Properties.Name -contains 'id' -and $msg.id -eq $script:id) { return $msg }
    }
}
[void](Cmd @{ execute = "qmp_capabilities" })
$sw = [Diagnostics.Stopwatch]::StartNew()
$stamps = @()
for ($i = 0; $i -lt $Frames; $i++) {
    $ppm = (Join-Path $root ("{0}_{1:D3}.ppm" -f $Prefix, $i)).Replace([string][char]92, "/")
    [void](Cmd @{ execute = "human-monitor-command"; arguments = @{ "command-line" = "screendump $ppm" } })
    $stamps += ("{0:D3} {1}" -f $i, [int]$sw.Elapsed.TotalMilliseconds)
    Start-Sleep -Milliseconds $IntervalMs
}
$client.Close()
$stamps
