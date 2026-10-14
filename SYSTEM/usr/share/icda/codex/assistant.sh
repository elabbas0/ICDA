#!/bin/sh
# Invoked by assistantd through the existing static BusyBox. Prompts and
# model IDs are files, never shell code. Credentials stay in ICDA storage.
export HOME=/home TERM=dumb
export PATH=/bin:/usr/bin:/sbin SHELL=/bin/sh
export SSL_CERT_FILE=/etc/ssl/certs.pem CODEX_CA_CERTIFICATE=/etc/ssl/certs.pem
export TOKIO_WORKER_THREADS=2
export NO_COLOR=1
export CODEX_SQLITE_HOME=/tmp/ai/codex-state
mode=$1
codex=$2
# Store account/session state on the same ICDA filesystem as the official
# executable. It survives reboot and supports atomic auth-cache replacement.
system_root=${codex%/usr/bin/codex}
case "$system_root" in
    /linux|/volumes/fat32-*/linux) export CODEX_HOME="$system_root/var/lib/icda/codex" ;;
    *) echo 'Codex must be installed on the ICDA system partition'; exit 1 ;;
esac
mkdir -p "$CODEX_HOME" /home/icda /tmp/ai "$CODEX_SQLITE_HOME"
cd /home/icda || exit 1
exec > /tmp/ai/codex.log 2>&1
run() {
    if [ "$mode" = ask ]; then
        "$@" < /tmp/ai/codex.prompt &
    else
        "$@" &
    fi
    runner=$!
    echo "$runner" > /tmp/ai/codex.pid
    wait "$runner"
}
case "$mode" in
    version) run "$codex" --version ;;
    login) run "$codex" login --device-auth ;;
    status) run "$codex" login status ;;
    ask)
        if ! "$codex" login status; then
            echo 'Sign in with /codex login in the assistant first.'
            echo 1 > /tmp/ai/codex.exit
            exit 1
        fi
        model=$(cat /tmp/ai/codex.model)
        # The user authorized ICDA system tools with full access. ICDA does
        # not implement the Linux namespaces needed by the CLI sandbox.
        set -- exec --skip-git-repo-check --json --sandbox danger-full-access \
            -c 'approval_policy="never"' \
            --output-last-message /tmp/ai/codex.answer
        if [ -n "$model" ] && [ "$model" != auto ]; then set -- "$@" --model "$model"; fi
        run "$codex" "$@" -
        ;;
    *) echo 'Unknown Codex action'; echo 1 > /tmp/ai/codex.exit; exit 1 ;;
esac
result=$?
echo "$result" > /tmp/ai/codex.exit
exit "$result"
