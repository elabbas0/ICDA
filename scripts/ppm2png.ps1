param([string]$In, [string]$Out)
$bytes = [System.IO.File]::ReadAllBytes($In)
$i = 0
function Skip-Ws([byte[]]$b, [ref]$p) {
    while ($true) {
        $c = $b[$p.Value]
        if ($c -eq 35) { while ($b[$p.Value] -ne 10) { $p.Value++ }; $p.Value++ }
        elseif ($c -eq 32 -or $c -eq 9 -or $c -eq 10 -or $c -eq 13) { $p.Value++ }
        else { break }
    }
}
function Read-Token([byte[]]$b, [ref]$p) {
    Skip-Ws $b $p
    $s = ""
    while ($true) {
        $c = $b[$p.Value]
        if ($c -eq 32 -or $c -eq 9 -or $c -eq 10 -or $c -eq 13 -or $c -eq 35) { break }
        $s += [char]$c
        $p.Value++
    }
    return $s
}
$magic = Read-Token $bytes ([ref]$i)
if ($magic -ne 'P6') { throw "not P6: $magic" }
$w = [int](Read-Token $bytes ([ref]$i))
$h = [int](Read-Token $bytes ([ref]$i))
$maxv = [int](Read-Token $bytes ([ref]$i))
Skip-Ws $bytes ([ref]$i)
$pixStart = $i
Add-Type -AssemblyName System.Drawing
$bmp = New-Object System.Drawing.Bitmap($w, $h)
$rect = New-Object System.Drawing.Rectangle(0, 0, $w, $h)
$fmt = [System.Drawing.Imaging.PixelFormat]::Format24bppRgb
$bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $fmt)
$rowBytes = $bd.Stride
$buf = New-Object byte[] ($rowBytes * $h)
for ($y = 0; $y -lt $h; $y++) {
    for ($x = 0; $x -lt $w; $x++) {
        $src = $pixStart + ($y * $w + $x) * 3
        $dstRowOff = $y * $rowBytes + $x * 3
        $buf[$dstRowOff]     = $bytes[$src + 2]
        $buf[$dstRowOff + 1] = $bytes[$src + 1]
        $buf[$dstRowOff + 2] = $bytes[$src]
    }
}
[System.Runtime.InteropServices.Marshal]::Copy($buf, 0, $bd.Scan0, $buf.Length)
$bmp.UnlockBits($bd)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Output "saved $Out ($w x $h)"
