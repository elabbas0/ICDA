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
# Publishing writes one commit through a temporary index, linked to the prior
# OTA release so GitHub reuses unchanged WebKit and Codex blobs. The working
# tree and current branch are left alone.
set -e
cd "$(dirname "$0")/.."
. scripts/progress.sh
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
spin "kernel + apps" "$OUT.build.log" env MSYS_NO_PATHCONV=1 docker run --rm -v "$(pwd -W):/workspace" -w /workspace icda-toolchain \
    sh -c 'make > /tmp/b.log 2>&1 || { tail -20 /tmp/b.log; exit 1; }'
rm -f "$OUT.build.log"

echo "[ota] assembling $OUT"
rm -rf "$OUT"
mkdir -p "$OUT/EFI/ICDA"
cp kernel/install-kernel.bin "$OUT/EFI/ICDA/KERNEL.BIN"
cp kernel/fs/bootx64-install.efi "$OUT/EFI/ICDA/GRUBX64.EFI"
# every system file, one line of progress each: on a cold tree this is several
# hundred copies and the loop used to show nothing at all until it was done
pb_reset
SF_TOTAL=$(grep -v '^#' boot/system-files.txt | wc -l | tr -d ' ')
SF_DONE=0
grep -v '^#' boot/system-files.txt | while read -r dest src; do
    [ -n "$dest" ] || continue
    mkdir -p "$OUT/SYSTEM/$(dirname "$dest")"
    cp "$src" "$OUT/SYSTEM/$dest"
    SF_DONE=$((SF_DONE + 1))
    progress "system files" "$SF_DONE" "$SF_TOTAL" "$dest"
done
note "system files: $SF_TOTAL copied"

echo "[ota] manifest for $VERSION"
body=$(mktemp)
{
    echo "icda-ota 1"
    echo "version $VERSION"
    echo "notes $NOTES"
    # one process hashes them all (a size and a hash program per file was slow)
    # Perl reports each file's hash as it goes, so the bar has something real to
    # follow instead of sitting at zero through a few hundred megabytes
    (cd "$OUT" && find EFI SYSTEM -type f | LC_ALL=C sort) | perl -MDigest::SHA -ne \
        'chomp; my $f = "'"$OUT"'/$_"; print "file $_ ", -s $f, " ", Digest::SHA->new(256)->addfile($f, "b")->hexdigest, "\n"; print STDERR "$_\n"'
    # WebKit as one pack: its files back to back ("wfile REL SIZE SHA OFFSET"),
    # published as linux.pack.part0 ... - a few downloads instead of thousands
    # of HTTPS connections.  Updaters before 1.8.5 skip wpack / wfile lines.
    if [ -d "$LINUXROOT/usr/bin" ]; then
        set -- linux/usr/bin/icda-webkit=userspace/webkit/out/icda-webkit \
               linux/usr/lib/gstreamer-1.0/libgsticda.so=userspace/webkit/out/libgsticda.so \
               loose:linux/usr/bin/icda-llm=userspace/llm/out/icda-llm \
               loose:linux/usr/bin/icda-say=userspace/llm/out/icda-say \
               loose:linux/usr/bin/icda-stt=userspace/llm/out/icda-stt
        # The official Codex package changes independently of WebKit: its
        # files are published on their own (lfile lines, the big ones in
        # parts), so a Codex change does not make every system download the
        # whole WebKit pack again, and a WebKit change does not repack Codex.
        for f in bin/codex bin/codex-code-mode-host codex-package.json; do
            [ -f "$LINUXROOT/usr/$f" ] && set -- "$@" "loose:linux/usr/$f=$LINUXROOT/usr/$f"
        done
        for resources in "$LINUXROOT/usr/codex-path" "$LINUXROOT/usr/codex-resources"; do
            [ -d "$resources" ] || continue
            # Package paths are pinned and validated by prepare-codex.ps1.
            resource_list=$(find "$resources" -type f | LC_ALL=C sort)
            saved_ifs=$IFS
            IFS='
'
            for resource in $resource_list; do
                rel=${resource#"$LINUXROOT/"}
                set -- "$@" "loose:linux/$rel=$resource"
            done
            IFS=$saved_ifs
        done
        for context in "$LINUXROOT"/usr/share/icda/codex/*; do
            [ -f "$context" ] || continue
            set -- "$@" "loose:linux/usr/share/icda/codex/${context##*/}=$context"
        done
        for model in "$LINUXROOT"/usr/share/icda/models/*; do
            [ -f "$model" ] || continue
            case "$model" in *.gguf|*.bin) continue ;; esac
            set -- "$@" "loose:linux/usr/share/icda/models/${model##*/}=$model"
        done
        perl scripts/ota-webkit.pl "$LINUXROOT" "$CACHE" "$PART" "$@"
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
    pb_reset
    OBJS=$( (cd "$OUT" && find EFI SYSTEM manifest.txt -type f) | wc -l | tr -d ' ')
    OBJS_DONE=0
    (cd "$OUT" && find EFI SYSTEM manifest.txt -type f | LC_ALL=C sort) | while read -r rel; do
        blob=$(git hash-object -w "$OUT/$rel")
        GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,$rel"
        OBJS_DONE=$((OBJS_DONE + 1))
        progress "hashing" "$OBJS_DONE" "$OBJS" "$rel"
    done
    note "hashed $OBJS files"
    # WebKit: the pack in pieces, linux.pack.part0 ... (the host's file size limit)
    if grep -q '^wpack ' "$OUT/manifest.txt"; then
        PACK_SHA=$(awk '$1 == "wpack" { print $4 }' "$OUT/manifest.txt")
        PACK_PIECES=$(awk '$1 == "wpack" { print $5 }' "$OUT/manifest.txt")
        # the pieces' git objects are kept with the cached pack: hashed again
        # only when the pack changed or the objects are gone
        if [ -f "$CACHE/blobs.txt" ] && [ -f "$CACHE/blobs-stamp" ] &&
           [ "$(cat "$CACHE/blobs-stamp")" = "$PACK_SHA" ] &&
           [ "$(wc -l < "$CACHE/blobs.txt" | tr -d ' ')" = "$PACK_PIECES" ] &&
           (while read -r k blob; do git cat-file -e "$blob" 2>/dev/null || exit 1; done < "$CACHE/blobs.txt"); then
            echo "[ota] WebKit pieces: cached objects"
        else
            tmp=$(mktemp -d)
            split -b "$PART" -d -a 1 "$CACHE/linux.pack" "$tmp/p"
            # hashing the pieces is the longest part of a publish: a few hundred
            # megabytes of WebKit, and until now it said nothing while it ran
            pb_reset
            : > "$CACHE/blobs.txt"
            PIECES_TOTAL=$(ls "$tmp"/p* | wc -l | tr -d ' ')
            PIECES_DONE=0
            for p in "$tmp"/p*; do
                echo "${p##*/p} $(git hash-object -w "$p")" >> "$CACHE/blobs.txt"
                PIECES_DONE=$((PIECES_DONE + 1))
                progress "webkit pieces" "$PIECES_DONE" "$PIECES_TOTAL" \
                    "${p##*/p}  $(wc -c < "$p" | tr -d ' ') bytes"
            done
            printf '%s' "$PACK_SHA" > "$CACHE/blobs-stamp"
            note "webkit pieces: $PIECES_TOTAL hashed"
            rm -rf "$tmp"
        fi
        while read -r k blob; do
            GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,linux.pack.part$k"
        done < "$CACHE/blobs.txt"
        # files kept out of the pack (lfile lines): each at its own path
        # ("parts N": over the host's size limit, published as REL.part0 ...)
        [ -f "$CACHE/loose.txt" ] && while IFS="$(printf '\t')" read -r rel src parts; do
            if [ "${parts:-0}" -gt 0 ]; then
                tmp=$(mktemp -d)
                split -b "$PART" -d -a 1 "$src" "$tmp/p"
                for p in "$tmp"/p*; do
                    blob=$(git hash-object -w "$p")
                    GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,$rel.part${p##*/p}"
                done
                rm -rf "$tmp"
            else
                blob=$(git hash-object -w "$src")
                GIT_INDEX_FILE="$idx" git update-index --add --cacheinfo "100644,$blob,$rel"
            fi
        done < "$CACHE/loose.txt"
    fi
    tree=$(GIT_INDEX_FILE="$idx" git write-tree)
    rm -f "$idx"
    # Fetch only the current commit and trees. This gives pack-objects the
    # prior file list so unchanged multi-hundred-megabyte packages are not
    # resent when the OTA branch advances.
    git fetch --quiet --filter=blob:none --depth=1 origin \
        refs/heads/ota:refs/remotes/origin/ota
    parent=$(git rev-parse --verify refs/remotes/origin/ota)
    commit=$(GIT_AUTHOR_NAME=elabbas0 GIT_AUTHOR_EMAIL=arkanabak09@gmail.com \
             GIT_COMMITTER_NAME=elabbas0 GIT_COMMITTER_EMAIL=arkanabak09@gmail.com \
             git commit-tree "$tree" -p "$parent" -m "ICDA $VERSION OTA release" -m "$NOTES")
    # The push is the one step that runs for a long time over a slow link, and
    # it is the one step that does not need this shell: OTA_COMMIT_ONLY stops
    # here and leaves the commit in .verify/ota-commit, for the caller to push
    # with a native git (a Cygwin fork under load dies with "couldn't create
    # signal pipe", which is what a half-finished push looks like afterwards).
    if [ -n "${OTA_COMMIT_ONLY:-}" ]; then
        printf '%s' "$commit" > .verify/ota-commit
        echo "[ota] prepared $VERSION ($commit) - not pushed (OTA_COMMIT_ONLY)"
        exit 0
    fi
    spin "pushing" "$OUT.push.log" git push -f origin "$commit:refs/heads/ota"
    rm -f "$OUT.push.log"
    echo "[ota] published $VERSION ($commit)"
fi
