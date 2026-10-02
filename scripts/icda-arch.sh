#!/usr/bin/env sh
# scripts/icda-arch.sh — native Arch Linux workflow (no Docker needed).
#
# Mirrors the scripts/icda.cmd surface for Arch:
#   ./scripts/icda-arch.sh ready [--headless] [--uefi]   check, build, smoke, run qemu
#   ./scripts/icda-arch.sh check                          verify host dependencies
#   ./scripts/icda-arch.sh install                        sudo pacman -S the missing deps
#   ./scripts/icda-arch.sh build                          make kernel.iso kernel-usb.img
#   ./scripts/icda-arch.sh smoke                          build + QEMU smoke test
#   ./scripts/icda-arch.sh qemu [--headless] [--uefi] [--sdl|--gtk|--vnc]
#                                                         interactive QEMU run
#   ./scripts/icda-arch.sh clean                          make clean
#
# Run GUI targets (qemu/ready) as YOUR USER, never under sudo: root has
# no access to your Wayland/X11 session, so QEMU silently falls back to
# a VNC server ("VNC server running on ::1:5900") instead of a window.
# Only `install` needs root (it re-execs sudo itself).
#
# Speed: KVM is used automatically when /dev/kvm is accessible,
# otherwise QEMU falls back to TCG (slow but works).
set -eu

cd "$(dirname "$0")/.."

PKGS="qemu-system-x86 qemu-ui-gtk qemu-ui-sdl libisoburn mtools gptfdisk dosfstools edk2-ovmf nasm grub gcc make"
QEMU_BIN="qemu-system-x86_64"

# GUI actions must not run as root (no display access -> silent VNC).
require_user() {
    if [ "$(id -u)" = "0" ]; then
        echo "icda-arch: do not run '$1' as root (sudo) — QEMU cannot open" >&2
        echo "icda-arch: your display from a root session and falls back to VNC." >&2
        echo "icda-arch: run as your user instead; only 'install' needs sudo." >&2
        exit 1
    fi
}

find_ovmf() {
    for p in /usr/share/edk2-ovmf/x64/OVMF_CODE.fd \
             /usr/share/edk2-ovmf/x64/OVMF_CODE.4m.fd \
             /usr/share/OVMF/OVMF_CODE.fd; do
        if [ -f "$p" ]; then
            printf '%s' "$p"
            return 0
        fi
    done
    return 1
}

# Display backend selection. QEMU's default (GTK) is flaky on Wayland
# compositors such as Hyprland and, worse, QEMU silently falls back to
# a VNC server when no display opens. So: SDL on Wayland (solid
# Wayland backend), GTK elsewhere, explicit --sdl/--gtk/--vnc override.
# Prints e.g. `-display sdl` (empty for GTK default).
display_flag() {
    case "${BACKEND:-auto}" in
        sdl) printf '%s' '-display sdl' ;;
        gtk) printf '%s' '-display gtk' ;;
        vnc) printf '%s' '-display vnc=:0' ;;
        *) if [ "${XDG_SESSION_TYPE:-}" = "wayland" ]; then
               printf '%s' '-display sdl'
           fi ;;
    esac
}

# Fail fast when a local window is requested but no display session is
# reachable (instead of booting into an invisible desktop). Skipped for
# headless runs and explicit --vnc.
need_display() {
    case "${BACKEND:-auto}" in
        vnc) return 0 ;;
    esac
    case "$1" in
        *headless*) return 0 ;;
    esac
    if [ -n "${WAYLAND_DISPLAY:-}" ] && [ -S "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/$WAYLAND_DISPLAY" ]; then
        return 0
    fi
    if [ -n "${DISPLAY:-}" ]; then
        return 0
    fi
    echo "icda-arch: no local display found (WAYLAND_DISPLAY and DISPLAY are both unset)." >&2
    echo "icda-arch: run from a graphical terminal, or use 'qemu --headless' / '--vnc'." >&2
    if [ -z "${WAYLAND_DISPLAY:-}" ] && [ "${XDG_SESSION_TYPE:-}" != "wayland" ]; then
        echo "icda-arch: hint: 'echo \$XDG_SESSION_TYPE' should print x11 or wayland." >&2
    fi
    exit 1
}

# Extra make overrides: KVM accel + display backend when available,
# Arch OVMF path for UEFI.
# NOTE: the QEMU assignment contains spaces, so call sites must expand
# it quoted:  make target "$(qemu_assign)" $(ovmf_assign)
qemu_assign() {
    qemu_cmd="qemu-system-x86_64"
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        qemu_cmd="$qemu_cmd -accel kvm"
    fi
    if [ -n "$(display_flag)" ]; then
        qemu_cmd="$qemu_cmd $(display_flag)"
    fi
    printf '%s' "QEMU=$qemu_cmd"
}

ovmf_assign() {
    if [ "${1:-}" = "uefi" ]; then
        ovmf="$(find_ovmf || true)"
        if [ -n "$ovmf" ]; then
            printf '%s' "OVMF_CODE=$ovmf"
        fi
    fi
}

need() {
    missing=""
    for b in qemu-system-x86_64 xorriso mtools sgdisk mkdosfs nasm grub-mkrescue gcc make ld; do
        if ! command -v "$b" >/dev/null 2>&1; then
            missing="$missing $b"
        fi
    done
    printf '%s' "$missing"
}

action="${1:-ready}"
shift || true

case "$action" in
    check)
        missing="$(need)"
        if [ -z "$missing" ]; then
            echo "icda-arch: all dependencies present."
            if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
                echo "icda-arch: KVM acceleration available."
            else
                echo "icda-arch: no KVM access, QEMU will use TCG (slow)."
            fi
            ovmf="$(find_ovmf || true)"
            if [ -n "$ovmf" ]; then
                echo "icda-arch: OVMF at $ovmf"
            else
                echo "icda-arch: OVMF not found (UEFI runs need edk2-ovmf)."
            fi
            if [ -n "$(BACKEND=auto display_flag)" ]; then
                echo "icda-arch: display backend: $(BACKEND=auto display_flag | sed 's/-display //') (Wayland default)"
            else
                echo "icda-arch: display backend: gtk (default)"
            fi
        else
            echo "icda-arch: missing:$missing"
            echo "icda-arch: run ./scripts/icda-arch.sh install"
            exit 1
        fi
        ;;
    install)
        # shellcheck disable=SC2086
        sudo pacman -S --needed $PKGS
        ;;
    build)
        require_user build
        make kernel.iso kernel-usb.img "$(qemu_assign)" $(ovmf_assign)
        ;;
    smoke)
        require_user smoke
        make qemu-smoke "$(qemu_assign)" $(ovmf_assign)
        ;;
    qemu)
        require_user qemu
        target="qemu"
        uefi=""
        BACKEND="auto"
        for arg in "$@"; do
            case "$arg" in
                --headless) target="qemu-headless" ;;
                --uefi) target="qemu-uefi"; uefi="uefi" ;;
                --uefi-headless|--headless-uefi) target="qemu-uefi-headless"; uefi="uefi" ;;
                --sdl) BACKEND="sdl" ;;
                --gtk) BACKEND="gtk" ;;
                --vnc) BACKEND="vnc" ;;
            esac
        done
        if [ "$uefi" = "uefi" ] && ! find_ovmf >/dev/null 2>&1; then
            echo "icda-arch: OVMF firmware not found, run ./scripts/icda-arch.sh install"
            exit 1
        fi
        need_display "$target"
        if [ "$BACKEND" = "vnc" ]; then
            echo "icda-arch: VNC display selected — connect a viewer to ::1:5900"
        fi
        make "$target" "$(qemu_assign)" $(ovmf_assign "$uefi")
        ;;
    clean)
        make clean
        ;;
    ready)
        require_user ready
        uefi=""
        BACKEND="auto"
        for arg in "$@"; do
            case "$arg" in
                --uefi|--uefi-headless|--headless-uefi) uefi="uefi" ;;
                --sdl) BACKEND="sdl" ;;
                --gtk) BACKEND="gtk" ;;
                --vnc) BACKEND="vnc" ;;
            esac
        done
        missing="$(need)"
        if [ -n "$missing" ]; then
            echo "icda-arch: missing:$missing — installing."
            # shellcheck disable=SC2086
            sudo pacman -S --needed $PKGS
        fi
        make kernel.iso kernel-usb.img "$(qemu_assign)" $(ovmf_assign)
        make qemu-smoke "$(qemu_assign)" $(ovmf_assign)
        echo "icda-arch: ready. Run ./scripts/icda-arch.sh qemu [--headless] [--uefi] [--sdl|--gtk|--vnc]"
        echo "icda-arch: in the shell, run '/apps/nptest.app' via: run /apps/nptest.app"
        ;;
    *)
        echo "Usage: scripts/icda-arch.sh [ready|check|install|build|smoke|qemu|clean] [--headless] [--uefi] [--sdl|--gtk|--vnc]"
        exit 2
        ;;
esac
