#!/bin/sh
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
export CODEX_HOME="$runtime/var/lib/icda/codex"
mkdir -p "$CODEX_HOME" "$CODEX_SQLITE_HOME"
for file in AGENTS.md CURRENT_TASK.md; do
    if [ ! -f "/home/$file" ]; then cp "/usr/share/icda/codex/$file" "/home/$file"; fi
done
exec "$runtime/usr/bin/codex" --no-alt-screen --search --sandbox danger-full-access --ask-for-approval never "$@"
