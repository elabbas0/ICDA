# ICDA Secure Workspace: design plan

Status: cipher design approved; open questions in §9 · Based on `origin/dev` at `3d7896d` (1.10.3) · Date: 2026-10-28

## 1. Goal

A networked workspace app. Several ICDA machines join one shared workspace and
read and write files in it. Every file is encrypted at rest and in transit,
using several layers of encryption inside a single file. The ICDA that creates
the workspace (the first connection) holds the highest permission, **Owner**.

Encryption must be fast enough for real-time use, and strong enough that
nobody, with any amount of computing power or AI, can brute-force it.

## 2. The cipher: what makes encryption uncrackable

### 2.1 Brute force is already impossible. AI doesn't change that

A 256-bit key has 2^256 ≈ 1.2 × 10^77 possible values. Suppose an attacker
could test 10^18 keys a second, far beyond any computer that exists. Searching
half the key space would still take about 10^51 years. The universe is about
10^10 years old.

AI doesn't speed that search up. Brute force is counting, and counting can't
be made clever. What AI *can* do is help an attacker find **design flaws**,
shortcuts that skip the search. So the defence against AI is the same as
against any strong attacker: use designs that have survived decades of public
attack and have no known shortcuts.

### 2.2 Why a brand-new cipher would make us weaker

A cipher earns trust from years of public cryptanalysis by thousands of
experts. AES (since 1998) and ChaCha20 (since 2008) have had that, and no
practical attack on either is known. A new ICDA cipher would have had none,
and unanalysed designs are exactly where AI-assisted analysis finds
shortcuts. Being new is a weakness here, not a strength.

### 2.3 Where real attacks happen, and our defence

Real systems are broken around the cipher, not through it.

| Real attack | Defence in ICDA |
|---|---|
| Predictable random numbers produce guessable keys | A kernel CSPRNG (RDSEED/RDRAND plus jitter feeding a ChaCha20 DRBG). **Today's code has this bug**; see §3. |
| Weak passwords protecting the keys | Argon2id (memory-hard, so GPUs and ASICs can't run millions of guesses), a minimum password strength, and a separate random 256-bit recovery key |
| Timing and cache side channels | Constant-time code only. AES uses AES-NI (no lookup tables); ChaCha20 is constant-time by design. |
| Nonce reuse (fatal for GCM and ChaCha20-Poly1305) | Fresh file key on every save, and per-chunk counter nonces |
| Future quantum computers | Hybrid key wrap: **X25519 + ML-KEM-768** (NIST FIPS 203). Grover's algorithm only halves symmetric strength, so a 256-bit key still gives about 128-bit security, which stays safe. |
| Stolen device or memory dump | Keystore sealed with the password, keys wiped after use, auto-lock |
| Tampering or file swapping | Every layer is authenticated, plus an Ed25519 signature over the whole file |

### 2.4 Our own construction: ICDA Layered Vault (`.icv`)

What is ours is the **construction**: how proven primitives are layered,
keyed and bound together. It's the same approach Signal, WireGuard and
VeraCrypt took.

| Layer | Primitive | Purpose |
|---|---|---|
| Key wrap | X25519 + ML-KEM-768 → HKDF-SHA256 → AES-256-GCM | A separate wrap of the file key for each member, and resistant to quantum attacks |
| Inner layer | ChaCha20-Poly1305 (256-bit key) | First authenticated encryption |
| Outer layer | AES-256-GCM (independent key) | Second authenticated encryption, from a different cipher family |
| Signature | Ed25519 | Proves who wrote the file and that nothing was changed |
| Keystore | Argon2id + AES-256-GCM | Protects each device's private keys with a passphrase |

Two independent ciphers from different families mean an attacker has to break
**both** to read a file. That's the real benefit of "multiple encryption in
one file".

**Decided 2026-10-28:** the vault uses the two-layer cascade, a hybrid
X25519 + ML-KEM-768 key wrap, and an Argon2id keystore. There will be no
ICDA-designed cipher layer for now. If one is added later, it must be an
extra layer with its own independent key, so it can't weaken the two proven
ones, and never the only layer.

### 2.5 Speed: real-time is not a problem

The figures below are rough single-core numbers for modern x86-64 hardware.
We'll measure on ICDA.

| Primitive | Approx. throughput |
|---|---|
| AES-256-GCM with AES-NI + PCLMULQDQ | several GB/s |
| ChaCha20-Poly1305 with SSE2/AVX2 | ~1–2 GB/s |
| Both layers in a cascade | well above 500 MB/s |

That's faster than the disk and the network, so encryption is never the
bottleneck. Key wrapping (X25519, ML-KEM, Ed25519) costs well under a
millisecond per file. Argon2id is deliberately slow (about 0.5 s), but it runs
only at unlock.

The existing `kernel/crypto/aes.c` is a scalar, table-based AES-128. It's slow
and leaks timing through the cache. The vault needs a new AES-NI path.

## 3. What GitHub `dev` already has (re-checked at 1.10.3)

| Area | Status on `origin/dev` | Work needed |
|---|---|---|
| RNG | `kernel/net/tls.c:880-900` still builds the X25519 private key and the client random from `sched_ticks()`. `kernel/proc/accounts.c:84` uses RDRAND but falls back to an rdtsc mix that isn't cryptographically secure. | **Critical:** one shared kernel CSPRNG and `getrandom`, used by TLS, accounts and the vault |
| Sockets | `SYS_NET` (`kernel/net/sock.h`) has TCP/UDP, connect, send, recv and poll | `sock_k_listen/accept` accepts only 127.0.0.0/8. Needs remote listeners that the workspace host can opt into. |
| Users | Accounts, sign-in, per-user file privacy, PBKDF2 (`kernel/proc/accounts.c`) | Bind a workspace identity to an ICDA user account |
| Ed25519, SHA-512 | `userspace/surfer/crypto/` | Move into a shared crypto library |
| X25519, SHA-256, HMAC, GCM | `kernel/crypto/` | Reuse. GCM must be extended to AES-256. |
| Missing | — | AES-256 (AES-NI), ChaCha20-Poly1305, HKDF, Argon2id, ML-KEM-768 |
| Multi-VM testing | QEMU user-mode networking blocks guest-to-guest traffic | A `make qemu-pair` target on a QEMU socket or mcast network |

## 4. Architecture

```
 ICDA-A (creator / Owner, hosts workspace)          ICDA-B, ICDA-C ... (members)
 ┌─────────────────────────────────┐               ┌──────────────────────────┐
 │ workspace.app (GUI on wm)       │               │ workspace.app            │
 │  ├─ vault lib  (.icv encode)    │               │  ├─ vault lib            │
 │  ├─ keystore   (identity keys)  │               │  ├─ keystore             │
 │  └─ wsd host   (stores blobs,   │◄── IWP over ──┤  └─ wsd client           │
 │                 enforces ACL)   │   TCP :7443   │                          │
 ├─────────────────────────────────┤  Noise_XX     ├──────────────────────────┤
 │ kernel: SYS_NET, CSPRNG         │  + ML-KEM     │ kernel: same             │
 └─────────────────────────────────┘               └──────────────────────────┘
```

- **Hub model.** The Owner's ICDA hosts the workspace and the others connect to
  it.
- **The host stores only ciphertext.** Clients encrypt before upload and
  decrypt after download. The host enforces the ACL on writes, and
  cryptography enforces it on reads: a member who isn't on a file's recipient
  list can't decrypt it.
- **Keys stay in the `workspace.app` process**, not in kernel memory.

## 5. Ownership and permissions

**The first ICDA is Owner, but ownership is bound to a key, not to who
connects first.** The creator generates an Ed25519 identity key and writes a
signed genesis record: `workspace_id, owner_pubkey, created_at`. Every later
permission chains back to that record by signature, so winning a race after a
reboot or spoofing an IP grants nothing. Ownership is transferred only by a
record signed by the current Owner.

| Role | Read | Write | Share/ACL | Add/remove members | Delete workspace / transfer ownership |
|---|---|---|---|---|---|
| **Owner** (creator, exactly one) | ✔ | ✔ | ✔ | ✔ (incl. Admins) | ✔ |
| **Admin** | ✔ | ✔ | ✔ | ✔ (Editors/Viewers) | ✘ |
| **Editor** | ✔ | ✔ | ✘ | ✘ | ✘ |
| **Viewer** | ✔ | ✘ | ✘ | ✘ | ✘ |

- A file can narrow access for itself, but never grant more than the member's
  role allows.
- **Membership log:** an append-only, hash-chained list of signed records.
  Every client checks it, so a compromised host can't forge a promotion.
- **Revocation:** removing a member re-keys every file they could read. Files
  they already downloaded can't be "un-seen", and the UI should say so.

## 6. `.icv` file format (v1)

```
Header (authenticated, plaintext)
  magic      "ICDAVLT1"            8 B
  version    u16
  suite      u16                  (1 = X25519+MLKEM768 / ChaCha20P1305 / AES256GCM / Ed25519)
  file_id    16 B random
  author     Ed25519 pubkey 32 B
  chunk_size u32                  (default 64 KiB)
  n_recip    u16
  recipients[n_recip]:
    member_key_id  8 B
    eph_x25519     32 B
    mlkem_ct       1088 B
    wrapped_fek    32 B + 16 B tag   (key = HKDF(x25519_ss || mlkem_ss, "icv-wrap"))
Body
  chunk[i] = AES256GCM_Kout( ChaCha20P1305_Kin( plaintext_i ) )
    nonce_i = chunk index (96-bit counter)
    AAD_i   = SHA256(header) || i || is_final
Footer
  Ed25519 signature over SHA256(header) || all chunk tags
```

The file key (FEK) is 32 random bytes from the CSPRNG and is new on every
save. `K_in, K_out = HKDF-SHA256(FEK, salt=file_id, info="icv-inner"/"icv-outer")`.
The chunk index and `is_final` flag stop chunks being reordered, truncated or
spliced in from another file.

## 7. Network protocol (IWP)

- **Transport:** TCP :7443 over `SYS_NET`. The handshake is Noise_XX_25519_ChaChaPoly_SHA256,
  with an ML-KEM-768 exchange mixed into the key (the same idea as PQ-WireGuard
  and Signal's PQXDH).
- **Joining:** the Owner shares an invite out of band:
  `icda-ws://<ip>:7443/<workspace_id>#<owner_fingerprint>.<one-time-token>`.
  The joiner pins the Owner's fingerprint, and the token is spent on first use.
- **Messages:** `HELLO`, `LOG_SYNC`, `LIST`, `GET`, `PUT`, `ACL_SET`,
  `MEMBER_ADD/ROLE/REMOVE`, `EVENT`, `BYE`.
- **Conflicts:** the host keeps the latest version. A stale PUT is rejected
  and the client keeps a conflict copy.

## 8. Implementation phases

0. **CSPRNG.** A kernel entropy pool (RDSEED → RDRAND → interrupt jitter)
   feeding a ChaCha20 DRBG, exposed as `SYS_GETRANDOM`. Switch `tls.c` and
   `accounts.c` over to it.
1. **Crypto library.** AES-256 (AES-NI) with GCM-256, ChaCha20-Poly1305,
   HKDF, Argon2id, ML-KEM-768, and Ed25519/SHA-512 moved out of Surfer. Each
   one checked against official test vectors (RFC 8439, RFC 5869, RFC 9106,
   FIPS 203, RFC 8032) and a boot-time self-test, plus a `cryptobench`
   throughput tool.
2. **Vault library and `vault` CLI.** `seal`, `open`, `inspect`, testable
   without any network.
3. **Identity and keystore.** Per-user Ed25519, X25519 and ML-KEM keys, sealed
   with Argon2id and linked to the ICDA account.
4. **Remote listeners.** Opt-in non-loopback `listen/accept` in `sock.c`, plus
   `make qemu-pair`.
5. **Noise + ML-KEM handshake and IWP.** The `wsd` host and client, with the
   invite flow.
6. **Permissions.** The membership log, roles, ACL, and re-keying on removal.
7. **GUI.** `workspace.app` on the window manager.
8. **Hardening.** Fuzz the parsers, check constant-time behaviour, rate-limit
   joins, and get an external review.

## 9. Open questions

1. **Owner offline:** read-only, or can an Admin host a signed replica?
2. **Scope:** LAN only, or across the internet (needs a relay or NAT traversal)?
3. **Files:** maximum size, and whether to keep version history.
4. **Owner key loss:** recovery through a printed backup key?
