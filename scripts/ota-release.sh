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
CACHE=.verify/ota-cache     # the WebKit pack and its manifest lines, reused while the root is unchanged
[ -n "$VERSION" ] || { echo "usage: $0 <version> \"<notes>\" [--publish]"; exit 2; }
[ -f "$KEY" ] || { echo "signing key $KEY missing"; exit 1; }
# a release without the WebKit root makes updaters remove WebKit from installed systems
[ -d "$LINUXROOT/usr/bin" ] || [ -n "${NO_WEBKIT:-}" ] || {
    echo "WebKit root $LINUXROOT missing: not publishing a release that removes WebKit (NO_WEBKIT=1 overrides)"; exit 1; }

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

echo "[ota] manifest for $VERSION"
body=$(mktemp)
{
    echo "icda-ota 1"
    echo "version $VERSION"
    echo "notes $NOTES"
    # one process hashes them all (a size and a hash program per file was slow)
    (cd "$OUT" && find EFI SYSTEM -type f | LC_ALL=C sort) | perl -MDigest::SHA -ne \
        'chomp; my $f = "'"$OUT"'/$_"; print "file $_ ", -s $f, " ", Digest::SHA->new(256)->addfile($f, "b")->hexdigest, "\n"'
    # WebKit as one pack: its files back to back ("wfile REL SIZE SHA OFFSET"),
    # published as linux.pack.part0 ... - a few downloads instead of thousands
    # of HTTPS connections.  Updaters before 1.8.5 skip wpack / wfile lines.
    if [ -d "$LINUXROOT/usr/bin" ]; then
        perl scripts/ota-webkit.pl "$LINUXROOT" "$CACHE" "$PART" \
            linux/usr/bin/icda-webkit=userspace/webkit/out/icda-webkit \
            linux/usr/lib/gstreamer-1.0/libgsticda.so=userspace/webkit/out/libgsticda.so \
            loose:linux/usr/bin/icda-llm=userspace/llm/out/icda-llm \
            loose:linux/usr/bin/icda-say=userspace/llm/out/icda-say \
            loose:linux/usr/bin/icda-stt=userspace/llm/out/icda-stt
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
    if grep -q '^wpack ' "$OUT/manifest.txt"; then
        # the pieces' git objects are kept with the cached pack: hashed again
        # only when the pack changed or the objects are gone
        if [ -f "$CACHE/blobs.txt" ] && (while read -r k blob; do git cat-file -e "$blob" 2>/dev/null || exit 1; done < "$CACHE/blobs.txt"); then
            echo "[ota] WebKit pieces: cached objects"
        else
            tmp=$(mktemp -d)
            split -b "$PART" -d -a 1 "$CACHE/linux.pack" "$tmp/p"
            for p in "$tmp"/p*; do echo "${p##*/p} $(git hash-object -w "$p")"; done > "$CACHE/blobs.txt"
            rm -rf "$tmp"
        fi
        while read -r k blob; do
            GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,linux.pack.part$k"
        done < "$CACHE/blobs.txt"
        # files kept out of the pack (lfile lines): each at its own path
        [ -f "$CACHE/loose.txt" ] && while IFS="$(printf '\t')" read -r rel src; do
            blob=$(git hash-object -w "$src")
            GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,$rel"
        done < "$CACHE/loose.txt"
    fi
    tree=$(GIT_INDEX_FILE="$idx" git write-tree)
    rm -f "$idx"
    commit=$(GIT_AUTHOR_NAME=elabbas0 GIT_AUTHOR_EMAIL=arkanabak09@gmail.com \
             GIT_COMMITTER_NAME=elabbas0 GIT_COMMITTER_EMAIL=arkanabak09@gmail.com \
             git commit-tree "$tree" -m "ICDA $VERSION OTA release" -m "$NOTES")
    git push -f origin "$commit:refs/heads/ota"
    echo "[ota] published $VERSION ($commit)"
fi
