# Milestone 06 — TLS and persistent pairing

## Status

The first candidate passed its principal X4/macOS physical gate: first pairing,
trusted reconnect, terminal traffic, runtime display commands, exit,
sleep/wake, diagnostics approval, and forget-all worked without a reported bug
or observable TLS lag. Runtime status reported 63,828 bytes free heap and a
45,800-byte minimum. It uses wolfSSL TLS 1.3 on the X4 and rustls TLS 1.3 on
the host, persistent self-generated P-256 identities, mutual certificate
possession checks, a six-digit fingerprint-derived SAS, host/device pins, and
unattended reconnect
for already paired terminal sessions. Diagnostics still requires approval.
Discovery stays plaintext and advertises `tls=required`; protocol payloads do
not.

The X4 stores one CRC-protected identity record and a CRC-protected fixed table
of four host fingerprints in NVS. The Rust host atomically stores its identity
and device certificate pins in a private per-user directory. An existing host
name with a changed key fails closed. First-pair state now uses a bounded
two-phase handoff: the host writes its device pin after ACCEPT, sends an empty
heartbeat only after that write succeeds, and the X4 commits the host pin only
after receiving that heartbeat. An interrupted handoff therefore re-prompts
instead of silently trusting one side. The waiting screen offers confirmed
forget-all plus a fixed-size paired-host list with confirmed per-host revoke.
Independent packet-capture evidence is deferred until a suitable capture
environment is available.

## Objective

Encrypt and authenticate v3 without changing its frame semantics. Preserve the
X4's physical approval model and support an unattended, already-paired daemon.

## Source anchors

- Plain TCP server: [`src/terminal/TerminalWifi.h`](../../src/terminal/TerminalWifi.h#L3)
- Connection lifecycle: [`src/terminal/TerminalWifi.cpp`](../../src/terminal/TerminalWifi.cpp#L32)
- wolfSSL build configuration: [`platformio.ini`](../../platformio.ini#L38)
- Existing outbound-only TLS wrapper:
  [`freeink-sdk/libs/network/SecureNet/include/SecureClient.h`](../../freeink-sdk/libs/network/SecureNet/include/SecureClient.h#L14)
- Existing TLS heap gate example:
  [`lib/KOReaderSync/KOReaderSyncClient.cpp`](../../lib/KOReaderSync/KOReaderSyncClient.cpp#L46)

## Threat model and locked requirements

Protect terminal contents and input from passive LAN observers and reject hosts
that were not paired. Pin a persistent per-device identity on the host and a
persistent host identity on the X4. Never ship one private key shared by every
firmware image, never use `setInsecure()` in release mode, and never label plain
TLS without identity verification as paired.

Initial pairing is interactive. Prefer a short authentication string derived
from the authenticated handshake transcript, displayed by both host and X4,
then confirmed physically. If the final implementation uses trusted-LAN TOFU
instead, label that weaker threat model explicitly and require the user to
confirm the device fingerprint once. Headless operation begins only after this
first pairing.

The X4 has no secure element and storage encryption has not been established;
document that persistent private material may not be protected against physical
SD/flash extraction.

## Architecture

The current `SecureClient` initiates outbound connections and cannot secure
`NetworkServer::accept()`. Add a narrowly scoped wolfSSL server-session wrapper
around an accepted `NetworkClient`, or move equivalent functionality into
`SecureNet`. Verify server APIs and compiled algorithms against the pinned
wolfSSL source before coding.

TLS wraps the accepted socket before the v3 greeting. Discovery may advertise a
non-secret identity fingerprint and `tls=required`, but discovery is not trusted
for authentication. After pairing, the Rust host verifies the pinned device and
the X4 verifies the pinned host credential. Plain v1/v2 is disabled by default
in secure builds; a plainly named development flag may re-enable it on trusted
networks.

## Resource plan

Measure free heap, minimum heap, largest allocatable block, handshake duration,
and steady-session heap before/after. Reuse the existing 2 KiB TLS-record strategy
where compatible. Do not run a TLS handshake concurrently with a display clean
or large temporary window allocation. Reject gracefully before allocation when
the measured heap gate is not met; never let `new` abort the firmware.

## Pairing lifecycle

- list and revoke paired hosts on-device;
- reject key changes for an existing host identity;
- explicit “forget all” action with confirmation;
- atomic credential write and recovery from interrupted write;
- no pairing material or session plaintext in logs/JSONL;
- rate-limit handshake and approval attempts.

## Verification gate

- Packet capture contains no terminal plaintext.
- Unknown host, wrong pin, expired/corrupt credential, replayed application
  frame, truncated TLS record, and key-change attempts fail closed.
- Paired reconnect works after host/device restart and preserves tmux behavior.
- Diagnostics still distinguishes `PRESENTED` and `READY`; TLS overhead is
  measured separately from display latency.
- Sleep/wake, exit, normal OTA recovery, free heap, and idle daemon CPU remain
  acceptable on the physical X4, macOS, and Linux.

## Current physical gate

1. Install the current Rust host before flashing the firmware.
2. Confirm discovery reports `TLS` as `required`.
3. Connect with `--host auto --verbose`; compare the six-digit code exactly and
   approve on the X4.
4. Run `knietty display status` and record `free_heap` and `min_free_heap`.
5. Exit and reconnect. It must authenticate and attach without a terminal
   approval prompt; diagnostics must still prompt.
6. Restart both sides and repeat. Then exercise X4 forget-all, delete the host
   device pin, and prove a fresh code/approval is required.
7. Capture the TCP stream and verify recognizable terminal text is absent.
8. Reject/rollback on reboot, OOM, handshake timeout, mismatched codes, lost
   input, sleep/wake regression, or failure to return to CrossPoint.

The original TLS candidate passed steps 1–6 except per-host revoke. The
two-phase commit, paired-host screen, revoke action, and visible forget hint
pass the full software matrix and firmware build but still need the compact
on-device follow-up gate before being called hardware-known-good.

## Complete when

A fresh pair requires human verification and a physical X4 confirmation;
subsequent paired reconnect is unattended and mutually authenticated; captures
show no plaintext; and resource measurements are recorded without regression.
