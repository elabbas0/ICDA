#!/bin/sh
# Builds Surfer's engine: WPE WebKit with libwpe and WPEBackend-fdo, for
# Alpine 3.20 (musl), the Linux root ICDA ships (tests/linux/mkroot.sh).
# Alpine 3.20 and 3.21 package WPE WebKit 2.40.5 (2023) and later Alpine
# none, so the engine is built here from the release tarballs.
#
#   sh userspace/webkit/engine/build.sh     -> userspace/webkit/out/engine-$WEBKIT.tar.gz
#
# The tarball is the engine's files under usr/ (libraries, processes,
# headers, pkg-config files), stripped; engine-$WEBKIT.deps lists the Alpine
# packages its libraries need.  Hours on four processors and about 2 GB of
# memory a job (JOBS=N to change the number), 15 GB of disk in
# userspace/webkit/out/engine-build, where a build that stopped goes on from
# where it was; skipped when the tarball exists.
#
# As Alpine builds WebKit 2.54 (aports community/webkit2gtk-6.0): clang,
# lld and ThinLTO, _FORTIFY_SOURCE off.  Left out, as ICDA has no use for
# them: the bubblewrap sandbox, WebDriver, speech, spelling, the
# accessibility bus, the new WPE Platform API (Surfer uses libwpe and
# WPEBackend-fdo, which WPE WebKit still supports as its legacy API).
set -e
cd "$(dirname "$0")/../../.."
. userspace/webkit/engine/versions.sh
OUT=userspace/webkit/out/engine-$WEBKIT.tar.gz
if [ -f "$OUT" ] && [ -z "$FORCE" ]; then
    echo "engine: $OUT is there (FORCE=1 to build it again)"
    exit 0
fi
mkdir -p userspace/webkit/out
MSYS_NO_PATHCONV=1 docker run --rm --dns 8.8.8.8 -e HTTPS_PROXY -e https_proxy -e JOBS="${JOBS:-4}" \
    -e WEBKIT=$WEBKIT -e LIBWPE=$LIBWPE -e FDO=$FDO \
    -v "$(pwd -W 2>/dev/null || pwd):/w" -w /w alpine:3.20 sh -c '
set -e
apk add --no-cache build-base clang lld llvm cmake samurai meson pkgconf bison flex gperf perl python3 ruby unifdef \
    curl patch glib-dev harfbuzz-dev icu-dev libjpeg-turbo-dev libepoxy-dev libgcrypt-dev libsoup3-dev libtasn1-dev \
    libxkbcommon-dev libxml2-dev libxslt-dev libpng-dev sqlite-dev libwebp-dev zlib-dev freetype-dev fontconfig-dev \
    woff2-dev lcms2-dev libavif-dev libjxl-dev hyphen-dev libdrm-dev mesa-dev mesa-gbm wayland-dev wayland-protocols \
    libseccomp-dev gstreamer-dev gst-plugins-base-dev gst-plugins-bad-dev openjpeg-dev expat-dev file pax-utils >/dev/null
B=/w/userspace/webkit/out/engine-build
rm -rf $B/stage
mkdir -p $B/src $B/stage
cd $B/src
get() {   # name, sha256
    [ -f "$1" ] || curl -sSfLO "https://wpewebkit.org/releases/$1"
    echo "$2  $1" | sha256sum -c -
    [ -d "${1%.tar.xz}" ] || tar -xf "$1"
}
get libwpe-$LIBWPE.tar.xz c880fa8d607b2aa6eadde7d6d6302b1396ebc38368fe2332fa20e193c7ee1420
get wpebackend-fdo-$FDO.tar.xz 544ae14012f8e7e426b8cb522eb0aaaac831ad7c35601d1cf31d37670e0ebb3b
get wpewebkit-$WEBKIT.tar.xz 787afd32ae23eac061621465390f44a462e874a9ebaba0b65521d7237fa8a5e4
# musl: ws-egl.cpp uses close() and ftruncate() without <unistd.h>
grep -q "include <unistd.h>" wpebackend-fdo-$FDO/src/ws-egl.cpp ||
    patch -d wpebackend-fdo-$FDO -p1 < /w/userspace/webkit/engine/patches/wpebackend-fdo-musl-unistd.patch
for p in libwpe-$LIBWPE wpebackend-fdo-$FDO; do
    [ -d $p/build ] || meson setup $p/build $p --prefix=/usr --libdir=lib --buildtype=release >/dev/null
    ninja -C $p/build >/dev/null
    DESTDIR=$B/stage ninja -C $p/build install >/dev/null
    ninja -C $p/build install >/dev/null       # and here, for WebKit to build against
done
export CC=clang CXX=clang++ AR=llvm-ar NM=llvm-nm RANLIB=llvm-ranlib
export LDFLAGS="-fuse-ld=lld -Wl,-O1,--as-needed"
export CFLAGS="-O2 -g0 -U_FORTIFY_SOURCE" CXXFLAGS="-O2 -g0 -U_FORTIFY_SOURCE"
export WEBKIT_NINJA_COMPILE_MAX=$JOBS
[ -f wk/build.ninja ] || cmake -S wpewebkit-$WEBKIT -B wk -G Ninja \
    -DPORT=WPE -DCMAKE_BUILD_TYPE=Release -DCMAKE_SKIP_RPATH=ON -DCMAKE_INSTALL_PREFIX=/usr -DCMAKE_INSTALL_LIBDIR=lib \
    -DLTO_MODE=thin \
    -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF -DENABLE_BUBBLEWRAP_SANDBOX=OFF \
    -DENABLE_WEBDRIVER=OFF -DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_SPELLCHECK=OFF -DENABLE_MINIBROWSER=OFF \
    -DENABLE_GAMEPAD=OFF -DENABLE_WPE_PLATFORM=OFF -DENABLE_WPE_QT_API=OFF -DENABLE_SAMPLING_PROFILER=OFF \
    -DUSE_ATK=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_SYSPROF_CAPTURE=OFF -DUSE_FLITE=OFF >/dev/null
ninja -C wk -j$JOBS | grep -v "^\[" || true
ninja -C wk -j$JOBS >/dev/null     # fails here when the build did
DESTDIR=$B/stage ninja -C wk install >/dev/null
cd $B/stage
find usr -type f \( -name "*.so*" -o -perm -u+x \) -exec sh -c "file \"\$1\" | grep -q ELF && strip --strip-unneeded \"\$1\"" _ {} \;
# the Alpine packages the libraries need (what is not the engine itself)
find usr -type f -name "*.so*" -o -type f -path "*/libexec/*" | xargs scanelf -nq -F "%n#F" 2>/dev/null | tr "," "\n" | sort -u |
    while read so; do [ -e usr/lib/$so ] && continue; apk info -qW /usr/lib/$so /lib/$so 2>/dev/null | head -1; done |
    sed "s/ is owned by.*//; s/-[0-9].*//" | sort -u > /w/userspace/webkit/out/engine-$WEBKIT.deps
tar -czf /w/userspace/webkit/out/engine-$WEBKIT.tar.gz usr
echo "engine: userspace/webkit/out/engine-$WEBKIT.tar.gz ($(du -h /w/userspace/webkit/out/engine-$WEBKIT.tar.gz | cut -f1)), needs: $(tr "\n" " " < /w/userspace/webkit/out/engine-$WEBKIT.deps)"
'
