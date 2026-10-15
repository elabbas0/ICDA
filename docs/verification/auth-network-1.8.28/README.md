# Authentication network investigation — ICDA 1.8.28

Tested on 15 October 2026 in an installed-layout QEMU WHPX VM, two CPUs,
3 GiB RAM, e1000 and user networking. The same FAT32 system disk and upload
test were used with the 1.8.27 and 1.8.28 kernels. No host account credentials
were imported for these tests.

## Confirmed TCP defect

After a partial acknowledgment, the TCP sender reduced its queued byte
count without moving the queue's starting position. Subsequent sends or
retransmissions could therefore use old bytes or overwrite bytes that the
peer had not acknowledged. That corrupts application data and can break
HTTPS requests. The new send-buffer head advances only by acknowledged
data bytes; writes and retransmissions use the same circular-buffer offset.

The regression program `tests/net-tcp-send.c` failed on 1.8.27. It passes
with the fix, including partial acknowledgments, appended writes,
retransmission, full drain, buffer wrap, and TCP sequence-number wrap.
The existing TCP receive regression also passed.

## Live comparison

The guest sent sixteen 65,536-byte deterministic payloads over one TCP
connection to a host fixture, which checked every byte independently.

| Kernel | Observed result |
| --- | --- |
| 1.8.27 | First upload corrupted, starting at byte offset 14,600; guest exit 1 |
| 1.8.28 | All 1,048,576 uploaded bytes verified across 16 writes; guest exit 0 |

[Before screenshot](upload-before.png), [after screenshot](upload-after.png),
and [fixture output](transport-results.txt).

To reproduce, compile `tests/linux/tcp-upload.c` as static musl, install
it in ICDA's SYSTEM/bin, and run `run /bin/tcp-upload`. QEMU's host address
defaults to `10.0.2.2`; the test accepts another address as its first argument.
Run the companion fixture on the host, for example:

```sh
docker run --rm -p 127.0.0.1:19080:19080 -e ICDA_TEST_BIND=0.0.0.0 \
  -v "$PWD:/workspace" -w /workspace python:3.12-alpine \
  python tests/linux/tcp-upload-server.py
```

Focused kernel tests:

```sh
gcc -I. -Ikernel -ffunction-sections -fdata-sections tests/net-tcp-send.c \
  -Wl,--gc-sections -o /tmp/icda-tcp-send
/tmp/icda-tcp-send
gcc -I. -Ikernel -ffunction-sections -fdata-sections tests/net-tcp-receive.c \
  -Wl,--gc-sections -o /tmp/icda-tcp-receive
/tmp/icda-tcp-receive
```

## Authentication scope

Official Codex CLI 0.162.1 reached the device website-and-code screen on
both kernels. Google's public sign-in form also rendered on both kernels;
the [1.8.28 screenshot](google-fixed.png) shows the empty sign-in form.
The reported device endpoint error and indefinitely loading Google page
were not reproduced in this VM. The physical hotspot was not available
for testing, and its exact terminal error was unavailable. The TCP defect
is confirmed independently, but these tests do not establish it as the
sole cause of those symptoms.

A plain Windows HTTP client received a Cloudflare browser challenge from
the device page, while the owner reported that the page opens on their
phone on the same connection. Device approval can therefore be completed
in the phone's browser once ICDA's CLI obtains the code. The relevant
OpenAI instructions are in the [authentication guide](https://learn.chatgpt.com/docs/auth).

Fresh account approval and token exchange require the owner. These tests
do not claim completed fresh sign-in or physical Wi-Fi verification. No
one-time codes, tokens, or raw authentication logs are published here.
