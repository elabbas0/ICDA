# ICDA development handoff

User requested a real OpenAI Codex assistant inside ICDA, signed in with their
ChatGPT account, selectable model, and BusyBox tool context. They also asked to
improve slow Wi-Fi on a 5 GHz iPhone hotspot and prepare an offline update.

The complete official Codex 0.162.1 Linux package is pinned and verified from
releases.openai.com. It includes the code-mode host, search tool, and required
resources. Its state is stored on ICDA's own partition, not Windows. The
assistant overlay has run a full Codex turn, executed `/bin/busybox uname -s`
and an echo marker, displayed the final response, and stayed alive through
shutdown after the Linux VM cleanup fix. The UI has also confirmed Codex
version and ChatGPT login status.

The latest guest regression ran 24 multithreaded `exit_group` rounds and
passed all 24; the parent reported exit status 0 and ICDA remained running.
The signal-thread regression passed 11/11, and the eventfd/epoll regression
passed 20/20. WebKit opened from a desktop double-click and rendered a live
HTTPS page.

Wi-Fi now scans 5 GHz channels, selects a less aggressive legacy transmit
rate, fixes AMRR retry accounting, and reassembles out-of-order TCP data. The
host simulation passed 74/74 rate cases and the TCP receive regression covers
reordering, overlaps, duplicates, wrap, FIN, and window bounds. The adapter
currently negotiates only legacy 802.11a/g, up to 54 Mbps; HT/VHT, 802.11ac,
and aggregation are not implemented. Measure real hotspot throughput after
booting ICDA on the actual Wi-Fi adapter before claiming a speed result.

The updater previously counted every pinned Qwen model as a required OS
download, causing a first update to request roughly 1.6 GB of model weights.
Model catalog entries are now excluded from OS update size and downloads;
users choose and fetch models over HTTP from the model picker.

Release status: public OTA 1.8.22 is live. It accidentally treats each pinned
Qwen weight as required during an OS update; the 1.5B model is over 1 GB.
Version 1.8.23 fixes that by keeping model rows in the signed catalog while
excluding them from the OS download plan. Rebuild and publish 1.8.23, then
regenerate the signed offline payload from the final sources. Keep it under
`.verify` and never include private auth.

Preserve the user's work and existing Windows data. The old dual-boot
installer is disabled after a Windows crash; never repartition, edit boot
entries, or rerun it. Use only the backed-up in-place ICDA update flow.
