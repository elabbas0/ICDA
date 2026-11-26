#!/bin/sh
# Tests ICDA's Windows layer (icda-win) on Linux, as it runs on ICDA's Linux
# personality: builds icda-win and the test programs (MinGW-w64) in an
# Alpine container, runs each program headless and checks what it does.
#   tests/winlayer/run.sh            all of them
# The programs also run on ICDA itself: copy the .exe files (left in
# tests/winlayer/out) to a disk and start them from Terminal.
cd "$(dirname "$0")/../.."
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache build-base mesa-dev mesa-osmesa libmspack-dev freetype-dev font-dejavu python3 mingw-w64-gcc >/dev/null
O=/w/tests/winlayer/out
mkdir -p $O
rm -f $O/icda-win
(cd userspace/winlayer && sh build-host.sh $O/icda-win) 2>&1 | grep -E "error|warning" || true
[ -x $O/icda-win ] || { echo "icda-win did not build"; exit 1; }
cd tests/winlayer
CC=x86_64-w64-mingw32-gcc
$CC -O1 -o $O/t_console.exe t_console.c
$CC -O1 -D__USE_MINGW_ANSI_STDIO=0 -o $O/t_console_ms.exe t_console.c
$CC -dumpspecs | sed "s/-lmsvcrt/-lucrt/g" > $O/ucrt.specs
$CC -O1 -specs=$O/ucrt.specs -D_UCRT -D__MSVCRT_VERSION__=0xE00 -o $O/t_console_ucrt.exe t_console.c
$CC -O1 -o $O/t_gl.exe t_gl.c -lopengl32 -lgdi32
$CC -O1 -o $O/t_proc.exe t_proc.c
x86_64-w64-mingw32-windres t_ver.rc -O coff -o $O/t_ver.res
$CC -O1 -o $O/t_ver.exe t_ver.c $O/t_ver.res -lversion
$CC -O2 -o $O/t_keys.exe t_keys.c -luser32 -lgdi32
$CC -O1 -o $O/t_dwrite.exe t_dwrite.c -ldwrite
$CC -O1 -o $O/t_barrier.exe t_barrier.c
$CC -O1 -o $O/t_stubs.exe t_stubs.c -ldsound -ldinput8 -ldxguid -lxinput1_4 -lshlwapi -lole32 -liphlpapi -lsecur32 -luuid
# the thread-local variables of t_console come from the winpthreads DLL of
# MinGW: a DLL of the program, loaded from its folder
cp /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll $O/
set +e
export HOME=/tmp/home ICDA_WIN_HEADLESS=1
fails=0
check() {   # name, condition
    if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi
}
cd $O
for exe in t_console.exe t_console_ms.exe t_console_ucrt.exe; do
    timeout 60 ./icda-win $exe one "two words" > $exe.out 2> $exe.err
    rc=$?
    diff -u /w/tests/winlayer/expected_console.txt $exe.out > $exe.diff
    d=$?
    [ $rc = 7 ] && [ $d = 0 ]; check "$exe (exit $rc)" $?
    [ $d = 0 ] || cat $exe.diff
done
ICDA_WIN_SHOT=$O/gl ICDA_WIN_SHOT_AT=3 timeout 60 ./icda-win t_gl.exe > t_gl.out 2>&1
grep -q "wglCreateContextAttribsARB found" t_gl.out && grep -q "^version [34]\.[0-9] renderer llvmpipe" t_gl.out &&
    grep -q "^error 0" t_gl.out && grep -q "^done" t_gl.out
check "t_gl.exe: OpenGL context, frames" $?
# the frame: blue with a red triangle in the middle
python3 -c "
import sys
d = open(\"gl-3.ppm\", \"rb\").read()
parts = d.split(b\"\n\", 3)
w, h = map(int, parts[1].split())
px = parts[3]
def at(x, y):
    i = (y * w + x) * 3
    return tuple(px[i:i + 3])
r, g, b = at(w // 2, h // 2)
r2, g2, b2 = at(2, 2)
sys.exit(0 if r > 200 and b < 60 and b2 > 180 and r2 < 80 else 1)
"
check "t_gl.exe: what was drawn" $?
timeout 60 ./icda-win t_proc.exe > t_proc.out 2>&1
grep -q "^dword 42, 2 values" t_proc.out && grep -q "^child read \[hello from parent\], args \[two words\] \[last\]" t_proc.out &&
    grep -q "^child exit code 70000" t_proc.out && grep -q "^after delete open=2" t_proc.out
check "t_proc.exe: registry, CreateProcess, exit code" $?
timeout 60 ./icda-win t_ver.exe > t_ver.out 2>&1
grep -q "^file version 1.2.3.4" t_ver.out && grep -q "^product version 1.2.3.4" t_ver.out
check "t_ver.exe: version resource" $?
# keys from a script: A pressed and released, then Q (which quits)
ICDA_WIN_INPUT="1:kd1e,1:ku1e,1:cha,1:kd10" ICDA_WIN_SHOT=$O/keys ICDA_WIN_SHOT_AT=1 timeout 60 ./icda-win t_keys.exe > t_keys.out 2>&1
sed "s/^ *[0-9]* //" t_keys.out | grep -v "^\[win\]" > t_keys.got
printf "ACTIVATE 1\nSETFOCUS\nwindow up\nKEYDOWN vk 41 sc 1E\nKEYUP vk 41 sc 1E\nCHAR 97 '"'"'a'"'"'\nKEYDOWN vk 51 sc 10\ndone\n" > t_keys.want
diff -u t_keys.want t_keys.got; check "t_keys.exe: keys" $?
# its first frame has its text (green) drawn with ICDA fonts
python3 -c "
import sys
d = open(\"keys-1.ppm\", \"rb\").read()
parts = d.split(b\"\n\", 3)
px = parts[3]
green = sum(1 for i in range(0, len(px) - 2, 3) if px[i + 1] > 200 and px[i] < 180)
sys.exit(0 if green > 200 else 1)
"
check "t_keys.exe: GDI text" $?
# DirectWrite: the system fonts (DejaVu, and ICDA'"'"'s Inter as Segoe UI), glyphs,
# outlines, coverage, fallback, and a font loaded from memory
mkdir -p fonts && cp /w/resources/fonts/Inter-*.ttf fonts/
ICDA_WIN_FONTS=$O/fonts timeout 60 ./icda-win t_dwrite.exe /usr/share/fonts/dejavu/DejaVuSans.ttf > t_dwrite.out 2>&1
grep -q "^segoe ui -> Inter" t_dwrite.out && grep -q "^arial bold italic -> DejaVu Sans weight 700" t_dwrite.out &&
    grep -q "^fallback 05D0 -> DejaVu Sans length 2" t_dwrite.out && grep -q "^memory font DejaVu Sans" t_dwrite.out &&
    grep -q "^done, 0 failed" t_dwrite.out
check "t_dwrite.exe: fonts, glyphs, fallback" $?
grep "^FAIL" t_dwrite.out
# what the layer lacks fails as its callers expect
timeout 60 ./icda-win t_stubs.exe > t_stubs.out 2>&1
grep -q "^done, 0 failed" t_stubs.out
check "t_stubs.exe: missing functions fail correctly" $?
grep "^FAIL" t_stubs.out
# bad pointers passed to the API fail the call, as on Windows
timeout 60 ./icda-win t_barrier.exe > t_barrier.out 2>&1
grep -q "^done, 0 failed" t_barrier.out
check "t_barrier.exe: bad pointers fail the call" $?
grep "^FAIL" t_barrier.out
echo "$fails failed"
exit $fails
'
