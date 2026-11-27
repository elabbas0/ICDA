#!/bin/sh
# Checks that a Linux root and the programs shipped beside it work together:
# what a release puts on every system that updates (scripts/ota-release.sh
# ships the root as the WebKit pack, and Surfer, the Windows layer and the
# sound output as files of their own; the updater installs them in one step).
#
#   tests/linux/check-root.sh [root] [file shipped beside it ...]
#
#   - the root's WebKit is the engine Surfer was built against
#     (userspace/webkit/out/icda-webkit.engine, written by userspace/webkit/build.sh):
#     Surfer built for one engine does not start with another;
#   - every program and library in the root, and each file given, finds the
#     libraries it needs in the root (the root's links are listed in .symlinks).
# The root is a path inside the repository (default .verify/linuxroot).
cd "$(dirname "$0")/../.."
ROOT=${1:-.verify/linuxroot}
[ $# -gt 0 ] && shift
[ -d "$ROOT/usr/lib" ] || { echo "check-root: $ROOT is not a Linux root (tests/linux/mkroot.sh makes it)"; exit 1; }
fail=0

# Surfer and the engine
lib=$(ls "$ROOT"/usr/lib/libWPEWebKit-2.0.so.1.* 2>/dev/null | head -1)
stamp=userspace/webkit/out/icda-webkit.engine
if [ -z "$lib" ]; then
    echo "check-root: no WebKit in $ROOT"; fail=1
elif [ ! -f "$stamp" ]; then
    echo "check-root: $stamp missing: Surfer was not built by userspace/webkit/build.sh"; fail=1
else
    read -r built_version built_sha < "$stamp"
    root_sha=$(sha256sum "$lib" | cut -d' ' -f1)
    if [ "$root_sha" != "$built_sha" ]; then
        echo "check-root: Surfer was built against WebKit $built_version, and $ROOT has another"
        echo "            (tests/linux/mkroot.sh makes the root again with the engine Surfer was built against)"
        fail=1
    else
        echo "check-root: WebKit $built_version in the root is the one Surfer was built against"
    fi
fi

# every program's libraries
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w -e ROOT="$ROOT" alpine:3.20 sh -c '
apk add --no-cache pax-utils >/dev/null
R=/w/$ROOT
# what the loader finds: files in /lib and /usr/lib, and links whose target is there
{ ls "$R/lib" "$R/usr/lib" 2>/dev/null
  [ -f "$R/.symlinks" ] && while IFS="	" read -r l t; do
      case "$l" in /lib/*|/usr/lib/*) [ -e "$R$t" ] && echo "${l##*/}";; esac
  done < "$R/.symlinks"; } | sort -u > /tmp/have
# or in the folders a program names itself (RPATH: pulseaudio, libproxy)
{ scanelf -R -B -F "%F;%r;%n" "$R"; for f in "$@"; do scanelf -B -F "%F;%r;%n" "/w/$f"; done; } 2>/dev/null |
while IFS=";" read -r file rpath needed; do
    for so in $(echo "$needed" | tr "," " "); do
        grep -qxF "$so" /tmp/have && continue
        found=
        for d in $(echo "$rpath" | tr ":" " "); do
            case "$d" in -) continue;; "\$ORIGIN"*) d="${file%/*}${d#\$ORIGIN}";; *) d="$R$d";; esac
            [ -e "$d/$so" ] && found=1 && break
            grep -q "^${d#$R}/$so	" "$R/.symlinks" 2>/dev/null && found=1 && break
        done
        [ -n "$found" ] || echo "check-root: ${file#/w/} needs $so, which the root does not have"
    done
done > /tmp/missing
n=$(wc -l < /tmp/missing)
cat /tmp/missing
echo "check-root: $(scanelf -R -B -F "%F" "$R" | wc -l) programs and libraries, and $# beside them: $n libraries missing"
[ "$n" = 0 ]
' sh "$@" || fail=1
exit $fail
