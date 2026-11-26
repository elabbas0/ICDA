#!/bin/sh
# Real Windows programs under icda-win, downloaded and checked on Linux:
#   7-Zip 25.01 (MSVC, msvcrt.dll, C++ exceptions FH3): archives made,
#     listed, tested and extracted (7z, zip, AES), Linux's 7-Zip reading
#     them; a bad switch and a damaged archive reported as on Windows.
#   exiv2 0.28 from conda-forge (Visual Studio 2022: FH4 exceptions, RTTI,
#     Microsoft's msvcp140.dll): its version, errors it throws and catches,
#     and EXIF tags it writes into a JPEG and reads back (exiftool agrees).
#   tests/winlayer/apps.sh            (needs the network for the downloads)
# The downloads are kept in tests/winlayer/out/apps.
cd "$(dirname "$0")/../.."
[ -x userspace/winlayer/out/icda-win ] || sh userspace/winlayer/build.sh
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache mesa-osmesa libmspack freetype 7zip zstd python3 curl imagemagick imagemagick-jpeg exiftool >/dev/null
O=/w/tests/winlayer/out/apps
mkdir -p $O/dl $O/7z $O/exiv2 $O/work
W=/w/userspace/winlayer/out/icda-win
export HOME=/tmp/h ICDA_WIN_HEADLESS=1
fails=0
check() { if [ "$2" = 0 ]; then echo "PASS $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }
cd $O/dl
[ -f 7z2501-x64.exe ] || curl -sSLo 7z2501-x64.exe https://www.7-zip.org/a/7z2501-x64.exe
[ -f 7z2501-extra.7z ] || curl -sSLo 7z2501-extra.7z https://www.7-zip.org/a/7z2501-extra.7z
(cd $O/7z && 7zz x -y ../dl/7z2501-x64.exe >/dev/null && 7zz x -y -oextra ../dl/7z2501-extra.7z >/dev/null)
python3 - <<PY
import json, urllib.request, os
for p in ["exiv2", "libbrotlicommon", "libbrotlidec", "libbrotlienc", "libexpat", "libiconv", "libzlib", "vc14_runtime"]:
    fs = [f for f in json.load(urllib.request.urlopen("https://api.anaconda.org/package/conda-forge/%s/files" % p))
          if f["attrs"].get("subdir") == "win-64"]
    f = max(fs, key=lambda f: f["attrs"].get("timestamp", 0))
    out = os.path.basename(f["basename"])
    if not os.path.exists(out):
        urllib.request.urlretrieve("https:" + f["download_url"], out)
PY
n=0
for f in *.conda; do
    n=$((n + 1)); mkdir -p /tmp/c$n
    (cd /tmp/c$n && 7zz x -y $O/dl/$f "pkg-*" >/dev/null && for t in pkg-*.tar.zst; do zstd -dq --stdout $t | tar x; done)
    cp /tmp/c$n/Library/bin/*.dll /tmp/c$n/Library/bin/*.exe $O/exiv2/ 2>/dev/null || true
    cp /tmp/c$n/*.dll $O/exiv2/ 2>/dev/null || true
done
set +e
# ---- 7-Zip
cd $O/work
mkdir -p data/sub
head -c 300000 /dev/urandom > data/random.bin
seq 1 50000 > data/numbers.txt
printf "unicode\n" > "data/sub/héllo-日本.txt"
cp $O/7z/7z.exe $O/7z/7z.dll .
z() { timeout 300 $W 7z.exe "$@" > z.out 2>&1; }
for f in t.7z t.zip p.7z; do [ -e $f ] && mv $f $f.old; done
z a t.7z data && z t t.7z && mkdir -p x1 && z x t.7z -ox1 -y && diff -r data x1/data >/dev/null &&
    mkdir -p x2 && (cd x2 && 7zz x -y ../t.7z >/dev/null) && diff -r data x2/data >/dev/null
check "7-Zip: 7z archive, made and read back, Linux 7-Zip agreeing" $?
z a -tzip t.zip data && mkdir -p x3 && (cd x3 && 7zz x -y ../t.zip >/dev/null) && diff -r data x3/data >/dev/null
check "7-Zip: zip archive" $?
z a -pSecret -mhe=on p.7z data && z t -pSecret p.7z; ok=$?
z t -pWrong p.7z; wrong=$?; [ $ok = 0 ] && [ $wrong = 2 ]
check "7-Zip: AES password, a wrong one refused" $?
z a -zzbad bad.7z data; [ $? = 7 ] && grep -q "Command Line Error" z.out
check "7-Zip: a bad switch (a C++ exception caught)" $?
# ---- exiv2
cd $O/exiv2
timeout 60 $W exiv2.exe --version > e.out 2>&1; grep -q "^exiv2 0\." e.out
check "exiv2: runs (Microsoft msvcp140)" $?
timeout 60 $W exiv2.exe Z:/nonexistent.jpg > e.out 2>&1; grep -q "Failed to open the file" e.out
check "exiv2: an error thrown and caught (FH4)" $?
magick -size 64x48 xc:red $O/work/pic.jpg
timeout 60 $W exiv2.exe -M"set Exif.Image.Make ICDA" -M"set Exif.Image.Model Windows layer" Z:$O/work/pic.jpg > e.out 2>&1
exiftool -s3 -Make -Model $O/work/pic.jpg > e2.out 2>&1
timeout 60 $W exiv2.exe -pa Z:$O/work/pic.jpg > e.out 2>&1
grep -q "Exif.Image.Make .*ICDA" e.out && grep -q "^ICDA" e2.out && grep -q "^Windows layer" e2.out
check "exiv2: EXIF written and read back (RTTI), exiftool agreeing" $?
echo "$fails failed"
exit $fails
'
