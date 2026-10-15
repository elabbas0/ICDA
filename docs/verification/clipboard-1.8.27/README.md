# Clipboard verification — ICDA 1.8.27

Tested on 15 October 2026 in the installed-layout QEMU WHPX VM used for
1.8.26, with the updated native Terminal and Surfer WebKit window.

ICDA already had a shared text clipboard (`/home/.clipboard`). Terminal
could paste but had no selection/copy support and discarded OSC clipboard
requests. Surfer's WebKit address bar did not handle clipboard shortcuts.

The update adds drag selection, Ctrl+Shift+C copying, and bounded OSC 52
clipboard writes. Ctrl+Shift+V pastes into Terminal, with queued input so
a partially accepted PTY write can finish on subsequent ticks. Terminal
shows these shortcuts below its text area. Surfer's address bar now handles
Ctrl+C, Ctrl+X, and Ctrl+V against the same clipboard.
The native shell disables a child's leftover bracketed-paste mode before
printing its own prompt, so interrupted CLI programs do not leave paste
markers in native commands.

## Observed results

- A program emitted an OSC 52 write; a separate process read back
  `ICDA_CLIPBOARD_1827_OK` from the shared clipboard.
- Dragging across `https://example.com/` in Terminal and pressing
  Ctrl+Shift+C copied the exact URL, verified by a separate reader.
  [Terminal screenshot](terminal-copy.png).
- In the actual WebKit Surfer window, Ctrl+L then Ctrl+V pasted that URL
  into the address bar. Enter loaded the Example Domain page.
  [Address-bar screenshot](surfer-paste.png).
- In official Codex CLI 0.162.1, the ChatGPT browser-login screen's `c`
  shortcut copied its full 462-byte OAuth URL. A separate reader verified
  its prefix and length without printing the private URL.
  [Codex clipboard check](codex-copy.png).
- A child enabled bracketed paste, copied `echo ICDA_TERMINAL_PASTE_OK`,
  and exited without disabling the mode. After returning to the native
  prompt, Ctrl+Shift+V followed by Enter executed the plain command and
  printed `ICDA_TERMINAL_PASTE_OK`, without stray bracket markers.
  [Native paste screenshot](terminal-paste.png).

The fresh OAuth token exchange still requires the account owner's browser
approval. These are clipboard tests, not a claim of completed fresh login.

The focused terminal regression program passed:

```sh
gcc -I. -ffunction-sections -fdata-sections tests/terminal-clipboard.c \
  -Wl,--gc-sections -o /tmp/icda-terminal-clipboard
/tmp/icda-terminal-clipboard
```

It checks BEL/ST-terminated OSC 52, malformed and oversized input, ignored
clipboard queries, clearing, reversed selections, hard line breaks,
soft-wrapped URLs, and UTF-8. The native kernel/apps and Alpine WebKit
window builds completed successfully.

The updated browser window ships separately as `linux/usr/bin/icda-surfer`.
The native launcher prefers it and retains `icda-webkit` compatibility.
The existing 550,141,145-byte WebKit pack has the same SHA-256 as 1.8.26:
`110e459f00f50e3c491e22395b01ffa3a2fe354f004bea479b5e75e8de37712a`.
Its unchanged libraries do not need downloading for this update.
