#!/usr/bin/env sh
set -eu

image="${ICDA_DOCKER_IMAGE:-icda-toolchain}"
action="${1:-ready}"
shift || true

docker_image() {
    docker build -t "$image" .
}

docker_make() {
    target="${1:-}"
    if [ -n "$target" ]; then
        docker run --rm -v "$PWD:/workspace" -w /workspace "$image" make "$target"
    else
        docker run --rm -v "$PWD:/workspace" -w /workspace "$image" make
    fi
}

docker_qemu() {
    target="$1"
    if [ -t 0 ]; then
        docker run --rm -it -v "$PWD:/workspace" -w /workspace "$image" make "$target"
    else
        docker run --rm -v "$PWD:/workspace" -w /workspace "$image" make "$target"
    fi
}

case "$action" in
    ready)
        docker --version
        docker info >/dev/null
        docker_image
        docker_make
        docker_make qemu-smoke
        ;;
    check)
        docker --version
        docker info >/dev/null
        ;;
    image)
        docker_image
        ;;
    build)
        docker_image
        docker_make
        ;;
    smoke)
        docker_image
        docker_make qemu-smoke
        ;;
    qemu)
        target="qemu"
        for arg in "$@"; do
            case "$arg" in
                --headless) target="qemu-headless" ;;
                --uefi) target="qemu-uefi" ;;
                --uefi-headless|--headless-uefi) target="qemu-uefi-headless" ;;
            esac
        done
        docker_image
        docker_qemu "$target"
        ;;
    clean)
        docker_image
        docker_make clean
        ;;
    install)
        echo "Install Docker Desktop on Windows with scripts/icda.cmd install, or install Docker Engine for this Linux distribution."
        ;;
    *)
        echo "Usage: scripts/icda.sh [ready|check|image|build|smoke|qemu|clean]"
        exit 2
        ;;
esac
