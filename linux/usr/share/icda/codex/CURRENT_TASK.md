# Handoff from ICDA development

User requests: add real OpenAI Codex to the ICDA assistant using their
ChatGPT/Codex account, allow model selection, provide BusyBox tool context,
then investigate and fix slow Wi-Fi on their current 5 GHz iPhone hotspot.
They also requested an offline update of the installed ICDA system.

Hardware: Intel Dual Band Wireless-AC 8260. Windows sees the iPhone hotspot
on channel 149 using 802.11ac, with a healthy signal and high negotiated rate.
ICDA's current station stack only negotiates legacy 802.11a/g rates; full
HT/VHT negotiation and aggregation remain unimplemented. Do not equate
5 GHz discovery with 802.11ac throughput.

Previous fixes in the working tree cover dual-band scanning, stable DHCP
transaction IDs, longer DHCP waits, native assistant OSABI, ICDA-owned Qwen
models fetched directly from Hugging Face, HDA routing, signal-strength arcs,
and mouse sensitivity/smoothing/acceleration. Preserve these changes.

The current TCP receiver was found to discard all out-of-order segments.
A bounded reassembly fix now retains them within the advertised receive
window and acknowledges the contiguous prefix. Host regression tests exercise
reordering, overlaps, duplicate packets, sequence/ring wrap, FIN, and window
limits. Physical hotspot throughput still requires measurement after reboot.

The Windows workspace is C:\Users\elabbas\Desktop\ICDA, branch dev. Existing
signed OTA release is 1.8.21. The dual-boot installer is disabled after an
earlier Windows crash: use an in-place backed-up offline update only, never
repartition or rerun that installer. Keep Windows/ESP/user data untouched.

Linux runtime repairs also correct read-only vDSO metadata, file-backed
page-prefetch offsets, independent epoll IN/OUT edges, and absolute futex
deadlines using the same clock as clock_gettime. Codex version and device
sign-in requests run inside the guest; full response/tool execution is still
being tested. Never claim that an unverified response worked.

The complete official Codex 0.162.1 Linux package now includes its code-mode
helper, search tool and resources, downloaded from releases.openai.com with
pinned SHA-256 verification. The ICDA assistant overlay has successfully run
/codex version and /codex status with ChatGPT sign-in. WebKit was verified
opening from a desktop double-click and rendering a live HTTPS page. The
remaining Codex startup hang is under test; per-thread signal masks and child
exit notification have been corrected and require guest regression checks.

ICDA 1.8.22 delivered these changes by OTA. The Codex package files are
published outside the WebKit pack, so Codex updates download only Codex.
