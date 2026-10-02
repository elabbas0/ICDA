<#
.SYNOPSIS
  Minimal QMP input driver: move the mouse, click, press keys, screendump.

.DESCRIPTION
  scripts/gui-check.py covers this but needs Python, which is not on this
  host.  Same QMP protocol and the same PS/2 workarounds:

    - the emulated mouse is PS/2, so it takes *relative* motion only
      (there is no "abs" input handler);
    - each packet carries a signed 8-bit delta, so a long move is walked
      in <=127 px steps;
    - x and y must be sent in *separate* input-send-event calls, or the
      y event is silently dropped;
    - QEMU can lose the first rel event after boot, so one warm-up
      jiggle is issued before the real move.

  The guest cursor position is tracked in gui-cursor.txt so successive
  calls accumulate deltas correctly (the same file gui-check.py uses).

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\qmp-input.ps1 -Port 4444 -Click 63,150 -Double -Shot term.png
#>
param(
    [int]$Port = 4444,
    [string]$Move = "",            # "x,y"
    [switch]$Click,
    [switch]$Double,
    [switch]$Right,
    [string]$Key = "",             # QEMU qcode, e.g. ret, esc, spc, t
    [string]$Shot = "",
    [int]$SettleMs = 4000,
    # Pacing between deltas.  gui-check.py sleeps 1.2 s per step because
    # the guest drops rel events that arrive faster than it can service
    # them; under TCG a much shorter delay silently loses motion and the
    # cursor lands somewhere else than requested.
    [int]$StepMs = 400,
    # PS/2 motion is lossy: a long move can land short, so a blind click
    # after -Move may miss its target.  -Sync re-reads the cursor out of
    # the framebuffer and corrects until it agrees, instead of trusting
    # gui-cursor.txt to still be accurate.
    [switch]$Sync
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$CursorFile = Join-Path $RepoRoot "gui-cursor.txt"

function Get-Cursor {
    if (Test-Path -LiteralPath $CursorFile) {
        try {
            $parts = (Get-Content -Raw $CursorFile).Trim() -split '\s+'
            if ($parts.Count -eq 2) { return @([int]$parts[0], [int]$parts[1]) }
        } catch { }
    }
    return @(512, 384)   # where the OS centers the pointer
}

<#
  Locate the pointer by differencing two consecutive screendumps.  On an
  idle desktop the pointer is the only thing that changes, so the pixels
  that differ are the old and new cursor positions - which is immune to
  the near-white *text* all over this UI, and does not depend on knowing
  where the guest thinks the cursor is.

  Returns a list of @{X;Y;N} clusters, largest first.
#>
function Find-CursorByDiff([string]$a, [string]$b) {
    $ba = [System.IO.File]::ReadAllBytes($a)
    $bb = [System.IO.File]::ReadAllBytes($b)
    if ($ba.Length -ne $bb.Length -or $ba.Length -lt 10) { return @() }
    if ($ba[0] -ne 0x50 -or $ba[1] -ne 0x36) { return @() }

    $i = 2
    function SkipWs2([byte[]]$bb2, [ref]$p) {
        while ($true) {
            $c = $bb2[$p.Value]
            if ($c -eq 35) { while ($bb2[$p.Value] -ne 10) { $p.Value++ }; $p.Value++ }
            elseif ($c -in 32, 9, 10, 13) { $p.Value++ }
            else { break }
        }
    }
    function Tok2([byte[]]$bb2, [ref]$p) {
        SkipWs2 $bb2 $p
        $s = ""
        while ($true) {
            $c = $bb2[$p.Value]
            if ($c -in 32, 9, 10, 13, 35) { break }
            $s += [char]$c; $p.Value++
        }
        return $s
    }
    $w = [int](Tok2 $ba ([ref]$i)); $h = [int](Tok2 $ba ([ref]$i))
    [void](Tok2 $ba ([ref]$i))
    SkipWs2 $ba ([ref]$i)
    $pix = $i

    # Bucket changed pixels into 16x16 cells, then merge adjacent cells
    # into clusters by flood fill over the occupied grid.
    $cw = 16
    $cols = [int][Math]::Ceiling($w / $cw)
    $rows = [int][Math]::Ceiling($h / $cw)
    $grid = New-Object 'int[,]' $rows, $cols
    for ([int]$y = 0; $y -lt $h; $y++) {
        [int]$gy = $y / $cw
        for ([int]$x = 0; $x -lt $w; $x++) {
            [int]$o = $pix + ($y * $w + $x) * 3
            if ($ba[$o] -ne $bb[$o] -or $ba[$o + 1] -ne $bb[$o + 1] -or $ba[$o + 2] -ne $bb[$o + 2]) {
                [int]$gx = $x / $cw
                $grid[$gy, $gx] = $grid[$gy, $gx] + 1
            }
        }
    }

    # Flood fill the occupied cells.
    $seen = New-Object 'bool[,]' $rows, $cols
    $clusters = @()
    for ([int]$gy = 0; $gy -lt $rows; $gy++) {
        for ([int]$gx = 0; $gx -lt $cols; $gx++) {
            if ($grid[$gy, $gx] -gt 0 -and -not $seen[$gy, $gx]) {
                $stack = New-Object System.Collections.Stack
                $stack.Push(@($gx, $gy))
                $seen[$gy, $gx] = $true
                $n = 0; $minx = $gx; $maxx = $gx; $miny = $gy; $maxy = $gy
                while ($stack.Count -gt 0) {
                    $c = $stack.Pop()
                    [int]$cx = $c[0]; [int]$cy = $c[1]
                    $n += $grid[$cy, $cx]
                    if ($cx -lt $minx) { $minx = $cx }; if ($cx -gt $maxx) { $maxx = $cx }
                    if ($cy -lt $miny) { $miny = $cy }; if ($cy -gt $maxy) { $maxy = $cy }
                    foreach ($d in @(@(1,0), @(-1,0), @(0,1), @(0,-1))) {
                        [int]$nx = $cx + $d[0]; [int]$ny = $cy + $d[1]
                        if ($nx -ge 0 -and $nx -lt $cols -and $ny -ge 0 -and $ny -lt $rows -and
                            $grid[$ny, $nx] -gt 0 -and -not $seen[$ny, $nx]) {
                            $seen[$ny, $nx] = $true
                            $stack.Push(@($nx, $ny))
                        }
                    }
                }
                $clusters += [pscustomobject]@{
                    X = [int](($minx + $maxx + 1) * $cw / 2)
                    Y = [int](($miny + $maxy + 1) * $cw / 2)
                    N = $n
                }
            }
        }
    }
    return @($clusters | Sort-Object -Property N -Descending)
}

<#
  Locate the pointer in a P6 screendump by looking for a near-white
  cluster.  This UI is full of light text, so this is only a fallback -
  prefer Find-CursorByDiff, which cannot be confused by text.
#>
function Find-CursorInPpm([string]$path) {
    $bytes = [System.IO.File]::ReadAllBytes($path)
    if ($bytes.Length -lt 10 -or $bytes[0] -ne 0x50 -or $bytes[1] -ne 0x36) { return $null }
    $i = 2
    function SkipWs([byte[]]$b, [ref]$p) {
        while ($true) {
            $c = $b[$p.Value]
            if ($c -eq 35) { while ($b[$p.Value] -ne 10) { $p.Value++ }; $p.Value++ }
            elseif ($c -in 32, 9, 10, 13) { $p.Value++ }
            else { break }
        }
    }
    function Token([byte[]]$b, [ref]$p) {
        SkipWs $b $p
        $s = ""
        while ($true) {
            $c = $b[$p.Value]
            if ($c -in 32, 9, 10, 13, 35) { break }
            $s += [char]$c; $p.Value++
        }
        return $s
    }
    $w = [int](Token $bytes ([ref]$i)); $h = [int](Token $bytes ([ref]$i))
    [void](Token $bytes ([ref]$i))     # maxval
    SkipWs $bytes ([ref]$i)
    $pix = $i
    # The ICDA cursor is a solid white arrow over a dark rim, so scan a
    # coarse grid for near-white clusters.  The arrow's tip is its
    # topmost-leftmost white pixel, so scanning top-down and keeping the
    # first strong block finds the tip rather than the wider tail.
    [int]$bestX = -1
    [int]$bestY = -1
    for ([int]$by = 0; $by -lt $h; $by += 8) {
        for ([int]$bx = 0; $bx -lt $w; $bx += 8) {
            [int]$hit = 0
            for ([int]$y = $by; $y -lt [Math]::Min($by + 8, $h); $y += 2) {
                for ([int]$x = $bx; $x -lt [Math]::Min($bx + 8, $w); $x += 2) {
                    [int]$o = $pix + ($y * $w + $x) * 3
                    if ($o -ge 0 -and ($o + 2) -lt $bytes.Length) {
                        if ($bytes[$o] -gt 235 -and $bytes[$o + 1] -gt 235 -and $bytes[$o + 2] -gt 235) { $hit++ }
                    }
                }
            }
            if ($hit -ge 2 -and $bestY -lt 0) {
                $bestY = $by
                $bestX = $bx
            }
        }
    }
    if ($bestX -lt 0) { return $null }
    return [pscustomobject]@{ X = [int]($bestX + 2); Y = [int]($bestY + 2) }
}

$client = New-Object System.Net.Sockets.TcpClient
try {
    $client.Connect("127.0.0.1", $Port)
    $stream = $client.GetStream()
    $reader = New-Object System.IO.StreamReader($stream)
    $writer = New-Object System.IO.StreamWriter($stream)
    $writer.AutoFlush = $true
    $writer.NewLine = "`n"

    $script:id = 0
    # NOTE: the parameter must not be called $args - that is a PowerShell
    # automatic variable, and shadowing it makes the value arrive as an
    # object[] that serializes to a JSON array, which QMP rejects with
    # "input member 'arguments' must be an object".
    function Send-Qmp([string]$exec, $qmpArgs) {
        $script:id++
        $obj = @{ execute = $exec; id = $script:id }
        if ($null -ne $qmpArgs) { $obj["arguments"] = $qmpArgs }
        $writer.WriteLine(($obj | ConvertTo-Json -Compress -Depth 10))
        while ($true) {
            $line = $reader.ReadLine()
            if ($null -eq $line) { throw "QMP connection closed" }
            $msg = $line | ConvertFrom-Json
            if ($msg.PSObject.Properties.Name -contains 'id' -and $msg.id -eq $script:id) {
                if ($msg.PSObject.Properties.Name -contains 'error') { throw "QMP error: $($msg.error.desc)" }
                return $msg
            }
        }
    }

    function Send-Rel([string]$axis, [int]$value) {
        $ev = @(@{ type = "rel"; data = @{ axis = $axis; value = $value } })
        [void](Send-Qmp "input-send-event" @{ events = $ev })
    }

    function Send-Btn([string]$button, [bool]$down) {
        $ev = @(@{ type = "btn"; data = @{ button = $button; down = $down } })
        [void](Send-Qmp "input-send-event" @{ events = $ev })
    }

    [void](Send-Qmp "qmp_capabilities" $null)

    if ($Move) {
        # Warm-up: QEMU sometimes drops the first rel event after boot.
        Send-Rel "x" -5; Start-Sleep -Milliseconds 200
        Send-Rel "x" 5;  Start-Sleep -Milliseconds 200

        $xy = $Move -split ','
        $tx = [int]$xy[0]; $ty = [int]$xy[1]
        $cur = Get-Cursor

        function Step-To([int]$tx2, [int]$ty2) {
            $c = Get-Cursor
            $dx = $tx2 - $c[0]; $dy = $ty2 - $c[1]
            while ($dx -ne 0 -or $dy -ne 0) {
                $sx = [Math]::Max(-127, [Math]::Min(127, $dx))
                $sy = [Math]::Max(-127, [Math]::Min(127, $dy))
                if ($sx -ne 0) { Send-Rel "x" $sx; $dx -= $sx; Start-Sleep -Milliseconds $StepMs }
                if ($sy -ne 0) { Send-Rel "y" $sy; $dy -= $sy; Start-Sleep -Milliseconds $StepMs }
            }
            "$($tx2) $($ty2)" | Set-Content -LiteralPath $CursorFile -Encoding ASCII
        }

        Write-Host "cursor $($cur[0]),$($cur[1]) -> $tx,$ty"
        Step-To $tx $ty

        if ($Sync) {
            # Measure where the guest really put the pointer instead of
            # trusting our arithmetic: jiggle by a known delta and diff
            # two frames.  The changed pixels are the old and new cursor
            # spots, so their centroid is our position plus half the
            # jiggle.  Immune to the light text all over this UI.
            $frameA = Join-Path $RepoRoot ".verify\_sync_a.ppm"
            $frameB = Join-Path $RepoRoot ".verify\_sync_b.ppm"
            New-Item -ItemType Directory -Force (Split-Path -Parent $frameA) | Out-Null
            function Dump-To([string]$path) {
                $q = ($path -replace '\\', '/') -replace ' ', '\ '
                if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Force }
                [void](Send-Qmp "human-monitor-command" @{ "command-line" = "screendump $q" })
                Start-Sleep -Milliseconds 700
                return (Test-Path -LiteralPath $path)
            }
            # A small jiggle is itself liable to be dropped by the PS/2
            # mouse, which reads as "no motion".  Use a delta large enough
            # to survive; the centroid correction scales with it.
            [int]$jig = 60
            for ($attempt = 1; $attempt -le 4; $attempt++) {
                if (-not (Dump-To $frameA)) { Write-Host "sync: screendump failed"; break }
                Send-Rel "x" $jig
                Start-Sleep -Milliseconds ($StepMs + 200)
                if (-not (Dump-To $frameB)) { Write-Host "sync: screendump failed"; break }

                $clusters = Find-CursorByDiff $frameA $frameB
                if (-not $clusters -or $clusters.Count -eq 0) {
                    Write-Host "sync: no motion seen; assuming the position is right"
                    break
                }
                $c = $clusters[0]
                [int]$px = $c.X - [int]($jig / 2)
                [int]$py = $c.Y
                Write-Host "sync: pointer actually at $px,$py (asked $tx,$ty)"
                $ex = [Math]::Abs($px - $tx); $ey = [Math]::Abs($py - $ty)
                if ($ex -le 8 -and $ey -le 8) { Write-Host "sync: converged"; break }
                "$px $py" | Set-Content -LiteralPath $CursorFile -Encoding ASCII
                Step-To $tx $ty
            }
        }
    }

    if ($Click -or $Double) {
        Send-Btn "left" $true;  Start-Sleep -Milliseconds 80
        Send-Btn "left" $false; Start-Sleep -Milliseconds 60
    }
    if ($Double) {
        # wm.c uses DBLCLICK_TICKS 40 at a 100 Hz tick, i.e. 400 ms
        # between the two button-down events.  Keep the whole pair well
        # inside that or the WM reads it as two single clicks.
        Start-Sleep -Milliseconds 60
        Send-Btn "left" $true;  Start-Sleep -Milliseconds 80
        Send-Btn "left" $false; Start-Sleep -Milliseconds 200
    }
    if ($Right) {
        $xy = $Right -split ','
        Send-Rel "x" 0
        Send-Btn "right" $true;  Start-Sleep -Milliseconds 60
        Send-Btn "right" $false; Start-Sleep -Milliseconds 120
        $xy = $Right -split ','
        $tx = [int]$xy[0]; $ty = [int]$xy[1]
        "$tx $ty" | Set-Content -LiteralPath $CursorFile -Encoding ASCII
    }
    if ($Key) {
        $k = @{ type = "key"; data = @{ key = @{ type = "qcode"; data = $Key }; down = $true } }
        $ku = @{ type = "key"; data = @{ key = @{ type = "qcode"; data = $Key }; down = $false } }
        [void](Send-Qmp "input-send-event" @{ events = @($k, $ku) })
        Write-Host "key $Key"
    }

    if ($Shot) {
        Start-Sleep -Milliseconds $SettleMs
        $outAbs = if ([System.IO.Path]::IsPathRooted($Shot)) { $Shot } else { Join-Path $RepoRoot $Shot }
        $qemuPath = ($outAbs -replace '\\', '/') -replace ' ', '\ '
        $ppm = [System.IO.Path]::ChangeExtension($outAbs, ".ppm")
        if (Test-Path -LiteralPath $ppm) { Remove-Item -LiteralPath $ppm -Force }
        [void](Send-Qmp "human-monitor-command" @{ "command-line" = "screendump $qemuPath" })
        Start-Sleep -Milliseconds 900
        if (-not (Test-Path -LiteralPath $ppm)) { throw "screendump produced no file" }
        & (Join-Path $RepoRoot "scripts\ppm2png.ps1") -In $ppm -Out $outAbs
    }
} finally {
    $client.Close()
}
