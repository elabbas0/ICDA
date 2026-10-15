# ChatGPT sign-in verification — ICDA 1.8.26

Tested on 15 October 2026 with the official Codex CLI 0.162.1 in an
installed-layout QEMU WHPX VM (two CPUs, 3 GiB RAM, e1000). Its separate
FAT32 system disk started without a Codex authentication cache. No host
ChatGPT credentials were imported for these tests.

## Reproduced failure

On 1.8.25, `codex login` returned `Not supported (os error 95)`. Selecting
**Sign in with ChatGPT** from plain `codex` reported that the login server
could not start, with the same OS error and code `-32603`.
[Menu failure screenshot](browser-before.png).

The Linux socket layer returned `EOPNOTSUPP` for TCP `listen` and `accept`.
Browser sign-in requires a local HTTP callback at `localhost:1455`.

## Change and observed results

The kernel now supports local IPv4 TCP bind/listen/accept, connection
queues, bidirectional buffered I/O, readiness notifications, write
half-close, and listener/peer cleanup. The Linux interface preserves bound
addresses and applies `accept4` flags. Listeners accept loopback clients;
this does not add externally accessible TCP servers.

- `tests/linux/tcp-loopback.c` passed **19/19 checks inside ICDA**, including
  duplicate-port rejection, epoll readiness, HTTP request/response,
  half-close, cleanup, and port reuse.
  [Socket test screenshot](socket-tests.png).
- A probe started the **actual official Codex login process**, requested
  `/` from its local callback server, and received
  `HTTP/1.1 404 Not Found`. The unrecognized route's 404 confirms that the
  server started and answered; it is not an OAuth sign-in success result.
  The probe then stopped only its own child login process.
  [Callback probe screenshot](callback-after.png).
- Selecting **Sign in with ChatGPT** in plain `codex` now reached the OAuth
  link screen without the previous login-server error.
- `codex login --device-auth` reached the official website-and-code screen
  on 1.8.25 and 1.8.26. The plain `codex` menu's **Sign in with Device Code**
  also reached that screen on 1.8.25. The reported device-code error was
  not reproduced. No one-time codes are published in this report.
- The existing TCP receive regression program passed. The kernel and OTA
  builds completed successfully.

The kernel read back from the tested FAT32 disk has SHA-256
`ea4fd96ec1774bdf47fabf0691c53ff52393c89a6475c1eb8a358becb5a9b3e9`,
matching the signed 1.8.26 OTA kernel.

These tests validate login startup and the callback transport. They do
not claim a completed fresh OAuth token exchange: that requires the
account owner to approve sign-in in their browser. Physical hotspot
behavior was not tested here. The existing nonfatal PATH-alias warning
remains.

## Reproduce

Compile the Linux integration tests with static musl, copy them into the
ICDA runtime or SYSTEM/bin, and run them in ICDA:

```sh
gcc -static -O2 tests/linux/tcp-loopback.c -o tcp-loopback
gcc -static -O2 tests/linux/codex-login-callback.c -o browser-probe
```

The callback probe requires `/bin/codex` and an unused port 1455. It saves
the login process output privately in `/tmp/codex-browser-probe.log` and
prints only the HTTP response status. Do not publish the raw login log.

For an account sign-in, run `codex login --device-auth` in ICDA and follow
the displayed instructions in the owner's browser. OpenAI documents
device-code login settings and ChatGPT authentication in its
[authentication guide](https://learn.chatgpt.com/docs/auth).
Browser-based `codex login` must return to a browser on ICDA itself;
opening that flow on a different machine returns to that machine's
localhost instead.
