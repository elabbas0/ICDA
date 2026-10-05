param([string]$Cmd = "info registers -a")
$client = New-Object System.Net.Sockets.TcpClient("127.0.0.1", 4444)
$stream = $client.GetStream()
$reader = New-Object System.IO.StreamReader($stream)
$writer = New-Object System.IO.StreamWriter($stream)
$writer.AutoFlush = $true
[void]$reader.ReadLine()
$writer.WriteLine('{"execute":"qmp_capabilities"}')
[void]$reader.ReadLine()
$obj = @{ execute = "human-monitor-command"; arguments = @{ "command-line" = $Cmd } }
$writer.WriteLine(($obj | ConvertTo-Json -Compress))
while ($true) {
    $line = $reader.ReadLine()
    if ($line -match '"return"') { ($line | ConvertFrom-Json).return; break }
}
$client.Close()
