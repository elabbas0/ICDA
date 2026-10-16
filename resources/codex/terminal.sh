#!/bin/busybox sh
# Interactive official Codex CLI; stdin/stdout stay attached to Terminal.
export HOME=/home PATH=/bin:/usr/bin:/sbin SHELL=/bin/sh TERM=xterm-256color
export SSL_CERT_FILE=/etc/ssl/certs.pem CODEX_CA_CERTIFICATE=/etc/ssl/certs.pem
export TOKIO_WORKER_THREADS=2 CODEX_SQLITE_HOME=/tmp/codex-state
runtime=
for root in /linux /volumes/fat32-*/linux; do
    case "$root" in /volumes/*) [ -f "${root%/linux}/ICDACFG.TXT" ] || continue ;; esac
    if [ -f "$root/usr/bin/codex" ]; then runtime=$root; break; fi
done
if [ -z "$runtime" ]; then echo 'Codex CLI is missing from the ICDA system partition.'; exit 1; fi
# Each user's sign-in stays in their own home: on the system partition every
# account could read it.  The owner's earlier sign-in moves here once.
export CODEX_HOME=/home/.codex
mkdir -p "$CODEX_HOME" "$CODEX_SQLITE_HOME"
legacy="$runtime/var/lib/icda/codex"
if [ "$(id -u)" = 1000 ] && [ -f "$legacy/auth.json" ] && [ ! -f "$CODEX_HOME/auth.json" ]; then
    for file in auth.json config.toml; do
        [ -f "$legacy/$file" ] && cp "$legacy/$file" "$CODEX_HOME/$file"
    done
    [ -f "$CODEX_HOME/auth.json" ] && rm -f "$legacy/auth.json"
fi
for file in AGENTS.md CURRENT_TASK.md; do
    if [ ! -f "/home/$file" ]; then cp "/usr/share/icda/codex/$file" "/home/$file"; fi
done
exec "$runtime/usr/bin/codex" --no-daemon --disable shell_snapshot --no-alt-screen --search --sandbox danger-full-access --ask-for-approval never "$@"
