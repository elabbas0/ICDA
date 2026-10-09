#!/bin/sh
# Builds an ICDA OTA release and (with --publish) pushes it to the `ota`
# branch of origin, where installed systems pick it up.
#
#   scripts/ota-release.sh <version> "<notes>" [--publish]
#
# Output: .verify/ota/  (EFI/ICDA/KERNEL.BIN, EFI/ICDA/GRUBX64.EFI, SYSTEM/...,
# linux/... (WebKit), manifest.txt).  The same tree is what a manual install
# copies to the ICDA partition, plus manifest.txt as SYSTEM/etc/icda-release.txt.
#
# The manifest is signed with the Ed25519 key in ~/.icda/ota-ed25519.pem
# (never committed); /sbin/updated only accepts manifests signed by it.
# Publishing writes a single orphan commit (force-pushed) through a temporary
# index, so the working tree and the current branch are left alone.
set -e
cd "$(dirname "$0")/.."
VERSION=$1
NOTES=$2
PUBLISH=$3
KEY="${USERPROFILE:-$HOME}/.icda/ota-ed25519.pem"
OUT=.verify/ota
# WebKit: the Linux root Surfer's engine runs from (tests/linux/mkroot.sh builds
# it); listed as "lfile" lines, files over PART bytes published in pieces
LINUXROOT=${LINUXROOT:-.verify/linuxroot}
PART=$((90 * 1024 * 1024))
[ -n "$VERSION" ] || { echo "usage: $0 <version> \"<notes>\" [--publish]"; exit 2; }
[ -f "$KEY" ] || { echo "signing key $KEY missing"; exit 1; }

echo "[ota] building"
powershell -NoProfile -Command "Get-Process qemu-system-x86_64 -EA SilentlyContinue | Stop-Process -Force" >/dev/null 2>&1 || true
MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/workspace" -w /workspace icda-toolchain \
    sh -c 'make > /tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }'

echo "[ota] assembling $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/EFI/ICDA"
cp kernel/install-kernel.bin "$OUT/EFI/ICDA/KERNEL.BIN"
cp kernel/fs/bootx64-install.efi "$OUT/EFI/ICDA/GRUBX64.EFI"
grep -v '^#' boot/system-files.txt | while read -r dest src; do
    [ -n "$dest" ] || continue
    mkdir -p "$OUT/SYSTEM/$(dirname "$dest")"
    cp "$src" "$OUT/SYSTEM/$dest"
done
if [ -d "$LINUXROOT" ]; then
    echo "[ota] adding WebKit from $LINUXROOT"
    cp -r "$LINUXROOT" "$OUT/linux"
    cp userspace/webkit/out/icda-webkit "$OUT/linux/usr/bin/icda-webkit"
    cp userspace/webkit/out/libgsticda.so "$OUT/linux/usr/lib/gstreamer-1.0/libgsticda.so" 2>/dev/null || true
    # names the kernel accepts in a patch: letters, digits and . _ - / + =
    (cd "$OUT" && find linux -type f | LC_ALL=C grep -v '^[A-Za-z0-9._/+=-]*$' | while read -r f; do rm -f "$f"; done)
fi

echo "[ota] manifest for $VERSION"
body=$(mktemp)
{
    echo "icda-ota 1"
    echo "version $VERSION"
    echo "notes $NOTES"
    (cd "$OUT" && find EFI SYSTEM -type f | LC_ALL=C sort) | while read -r rel; do
        size=$(wc -c < "$OUT/$rel" | tr -d ' ')
        sha=$(sha256sum "$OUT/$rel" | cut -d' ' -f1)
        echo "file $rel $size $sha"
    done
    # WebKit as one pack: its files back to back ("wfile REL SIZE SHA OFFSET"),
    # published as linux.pack.part0 ... - a few downloads instead of thousands
    # of HTTPS connections.  Updaters before 1.8.5 skip wpack / wfile lines.
    if [ -d "$OUT/linux" ]; then
        rm -f "$OUT/linux.pack"
        off=0
        (cd "$OUT" && find linux -type f | LC_ALL=C sort) | while read -r rel; do
            size=$(wc -c < "$OUT/$rel" | tr -d ' ')
            sha=$(sha256sum "$OUT/$rel" | cut -d' ' -f1)
            cat "$OUT/$rel" >> "$OUT/linux.pack"
            echo "wfile $rel $size $sha $off"
            off=$((off + size))
        done
        psize=$(wc -c < "$OUT/linux.pack" | tr -d ' ')
        echo "wpack linux.pack $psize $(sha256sum "$OUT/linux.pack" | cut -d' ' -f1) $(( (psize + PART - 1) / PART ))"
    fi
} > "$body"
openssl pkeyutl -sign -inkey "$KEY" -rawin -in "$body" -out "$body.sig"
{
    cat "$body"
    printf 'signature %s\n' "$(od -An -tx1 -v "$body.sig" | tr -d ' \n')"
} > "$OUT/manifest.txt"
rm -f "$body" "$body.sig"
mkdir -p "$OUT/SYSTEM/etc"
grep -c '^file ' "$OUT/manifest.txt" | sed 's/^/[ota] files: /'
du -sh "$OUT" | sed 's/^/[ota] size: /'

if [ "$PUBLISH" = "--publish" ]; then
    echo "[ota] publishing to origin/ota"
    idx=$(mktemp)
    rm -f "$idx"
    (cd "$OUT" && find EFI SYSTEM manifest.txt -type f | LC_ALL=C sort) | while read -r rel; do
        blob=$(git hash-object -w "$OUT/$rel")
        GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,$rel"
    done
    # WebKit: the pack in pieces, linux.pack.part0 ... (the host's file size limit)
    if [ -f "$OUT/linux.pack" ]; then
        tmp=$(mktemp -d)
        split -b "$PART" -d -a 1 "$OUT/linux.pack" "$tmp/p"
        for p in "$tmp"/p*; do
            k=${p##*/p}
            blob=$(git hash-object -w "$p")
            GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,linux.pack.part$k"
        done
        rm -rf "$tmp"
    fi
    tree=$(GIT_INDEX_FILE="$idx" git write-tree)
    rm -f "$idx"
    commit=$(GIT_AUTHOR_NAME=elabbas0 GIT_AUTHOR_EMAIL=arkanabak09@gmail.com \
             GIT_COMMITTER_NAME=elabbas0 GIT_COMMITTER_EMAIL=arkanabak09@gmail.com \
             git commit-tree "$tree" -m "ICDA $VERSION OTA release" -m "$NOTES")
    git push -f origin "$commit:refs/heads/ota"
    echo "[ota] published $VERSION ($commit)"
fi
