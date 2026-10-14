#!/bin/sh
# Runs the real static worker and GGUF. Run in a disposable Linux container.
set -eu
BIN=${1:-userspace/llm/out/icda-llm}
MODEL=${2:-.verify/linuxroot/usr/share/icda/models/qwen2.5-0.5b-instruct-q4_k_m.gguf}
mkdir -p /tmp/ai
printf '1\nsystem\037Answer briefly.\036user\037What is two plus two?\036' > /tmp/ai/local-request
"$BIN" -m "$MODEL" -t 1 -c 2048 &
worker=$!
trap 'kill "$worker" 2>/dev/null || true' EXIT
await_reply() {
    seq=$1
    for attempt in $(seq 1 120); do
        if grep -q "^$seq done$" /tmp/ai/local-reply 2>/dev/null; then
            test "$(wc -c < /tmp/ai/local-reply)" -gt 8
            cat /tmp/ai/local-reply
            return 0
        fi
        if grep -q ' error$' /tmp/ai/local-reply 2>/dev/null; then
            cat /tmp/ai/local-reply
            return 1
        fi
        kill -0 "$worker"
        sleep 1
    done
    cat /tmp/ai/llm.log
    return 1
}
await_reply 1
printf '2\nuser\037Reply with a short greeting.\036' > /tmp/ai/local-request.tmp
mv /tmp/ai/local-request.tmp /tmp/ai/local-request
await_reply 2
echo 'Local GGUF: cold request and second request passed'
