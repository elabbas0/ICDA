# ICDA current task

Codex is a standalone official CLI launched only from Terminal with `codex`.
It uses the owner's ChatGPT login, real BusyBox tools, live web search, and
full system-file access. It must not be reintroduced as an assistant provider.
Use the CLI's own model selector and sign-in flow.

The built-in assistant uses Qwen locally or a configured compatible endpoint.
Settings > Assistant selects Qwen 0.5B or 1.5B and downloads the selected
model directly from the pinned official Hugging Face HTTPS URL. `/model
install 0.5b`, `/model install 1.5b`, and `/model status` expose the same flow.
The installer verifies size and SHA-256 before publishing the downloaded file
on the ICDA partition. Updates and offline bundles do not install weights.

Linux runtime fixes cover vDSO metadata, file prefetch offsets, eventfd/epoll
notifications, signal masks and restart, futex deadlines, and safe process
VM reclamation. Earlier guest checks verified a real Codex answer and BusyBox
tools, 24/24 thread-exit cases, and WebKit opening a live HTTPS page.
The interactive CLI opened with ChatGPT sign-in and completed a live web tool
request and a BusyBox write/read on the ICDA volume. Extended termios2 passed
in the guest. The Assistant downloaded Qwen 0.5B from official Hugging Face;
its disk file matched the pinned SHA-256 and answered ICDA_QWEN_READY in a
3 GiB guest. A 1.5 GiB guest correctly reported insufficient model memory.
Focused tests cover reconnect/resume, rejected ranges and hashes, preserving
existing models on failure, updater exclusion, and streaming beyond 1 GiB
without a growing kernel RAM copy. Actual hotspot throughput remains unmeasured.

Wi-Fi has dual-band scanning, stable DHCP IDs, AMRR retry accounting and
TCP receive reassembly. The current driver supports only legacy 802.11a/g
up to 54 Mbps; HT/VHT and aggregation are unimplemented. Real hotspot speed
must be measured on hardware before claiming a throughput improvement.

Preserve personal data and credentials. Never import model weights from
Windows. The old dual-boot installer is disabled; use backed-up in-place
updates without repartitioning or changing boot registration.
