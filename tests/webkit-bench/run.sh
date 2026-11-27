#!/bin/sh
# Surfer on WebKit, measured before and after a change, on real web sites.
#
#   tests/webkit-bench/run.sh [before-revision] [out-dir] [campaign options]
#
# before-revision: what to compare against (default HEAD); "after" is the
# working tree.  Out: <out-dir>/campaign/*.json, summary.json, screenshots
# (default .verify/webkit-bench).  Campaign options go to campaign.py
# (--reps N, --only scenario,scenario).
#
# Needs Linux with Docker; the container runs privileged (perf counters and
# cgroups for the browser's processes).  Behind a TLS-inspecting proxy, set
# HTTPS_PROXY and BENCH_CA (the proxy's CA bundle).
set -e
cd "$(dirname "$0")/../.."
BEFORE=${1:-HEAD}
OUT=${2:-.verify/webkit-bench}
[ $# -gt 2 ] && shift 2 || set --
# Surfer's engine, as ICDA ships it (userspace/webkit/engine: built first when it is not, hours)
. userspace/webkit/engine/versions.sh
sh userspace/webkit/engine/build.sh
IMAGE=icda-webkit-bench-$WEBKIT
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
CA=${BENCH_CA:+-v $BENCH_CA:/etc/ssl/certs/ca-certificates.crt:ro}

# Alpine 3.20 as ICDA's Linux root: Surfer's engine, Mesa's software renderer, GStreamer
if ! docker image inspect $IMAGE >/dev/null 2>&1; then
    echo "[bench] making the image"
    docker rm -f icda-webkit-bench-setup >/dev/null 2>&1 || true
    docker run --name icda-webkit-bench-setup --network host -e HTTPS_PROXY -e https_proxy -e WEBKIT=$WEBKIT \
        -v "$PWD/userspace/webkit/out:/engine:ro" $CA alpine:3.20 sh -c '
        apk add --no-cache build-base pkgconf glib-dev libsoup3-dev libxkbcommon-dev wayland-dev mesa-dev \
            mesa-dri-gallium mesa-egl mesa-gles mesa-gbm font-dejavu font-noto fontconfig glib-networking gstreamer \
            gstreamer-tools gst-plugins-base gst-plugins-good gst-plugins-bad gst-libav python3 procps-ng \
            $(cat /engine/engine-$WEBKIT.deps) >/dev/null
        tar -xzf /engine/engine-$WEBKIT.tar.gz -C /'
    docker commit icda-webkit-bench-setup $IMAGE >/dev/null
    docker rm icda-webkit-bench-setup >/dev/null
fi

# the two browsers' sources
rm -rf "$OUT/src-before" "$OUT/src-after"
mkdir -p "$OUT/src-before" "$OUT/src-after"
git archive "$BEFORE" userspace | tar -x -C "$OUT/src-before"
cp -r userspace "$OUT/src-after/"

# perf counts system calls with the kernel's raw_syscalls:sys_enter tracepoint
[ -f /sys/kernel/tracing/events/raw_syscalls/sys_enter/id ] || mount -t tracefs nodev /sys/kernel/tracing 2>/dev/null || true
TP=$(cat /sys/kernel/tracing/events/raw_syscalls/sys_enter/id 2>/dev/null || echo 0)

echo "[bench] before: $BEFORE, after: the working tree; results in $OUT"
docker run --rm --privileged --cgroupns=host --network host --shm-size=2g \
    -v /sys/fs/cgroup:/sys/fs/cgroup:rw -v "$PWD/tests/webkit-bench:/bench-src:ro" -v "$OUT:/bench/out" $CA \
    -e HTTPS_PROXY -e https_proxy -e SYSCALL_TP="$TP" $IMAGE sh -c '
    set -e
    mkdir -p /bench && cp -r /bench-src/* /bench/
    sh /bench/build-in-container.sh /bench/out/src-before /bench/out/icda-webkit-before
    sh /bench/build-in-container.sh /bench/out/src-after /bench/out/icda-webkit-after
    gcc -O2 -shared -fPIC -o /bench/out/procemu.so /bench/host/procemu.c -ldl
    python3 /bench/campaign.py /bench/out/campaign "$@"
    python3 /bench/analyze.py /bench/out/campaign /bench/out/campaign/summary.json' sh "$@"
