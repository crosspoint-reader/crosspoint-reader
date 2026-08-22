# Milestone 05 — Rust host migration

## Objective

Implement the Linux/macOS host bridge in Rust with protocol, PTY, signal,
terminal-restoration, reconnect, discovery, diagnostics, and frozen JSONL
schema compatibility. Remove the superseded host only after both operating
systems pass.

## Source anchors

- Rust host package: [`host-rs`](../../host-rs)
- Protocol fixture: [`host-rs/fixtures/protocol-v3.frames`](../../host-rs/fixtures/protocol-v3.frames)
- Linux integration: [`host-rs/integration/linux`](../../host-rs/integration/linux)
- macOS integration: [`host-rs/integration/macos`](../../host-rs/integration/macos)
- Protocol v3 specification produced by Milestone 02.

## Implementation status

Milestone 05 is complete. The Rust host is the sole knietty host implementation.
The first slice provided the complete bounded v3 frame/diagnostics codec,
self-contained golden frames, repeated UDP discovery, a `list` command, and the
legacy `--list-devices` alias.

The second slice provides portable PTY creation, window sizing, controlling-TTY
and isolated process-group setup, `TERM`/`COLUMNS`/`LINES`, tmux/shell choice,
escalating child cleanup, and an RAII local-terminal guard. The guard restores
termios and descriptor flags on normal return and panic unwinding. A bounded
`pty-smoke` command exercises this layer without opening a network session.

The third slice ports v3/v2/v1 negotiation, host/time metadata, explicit target
resolution, auto-discovery ambiguity rejection, physical approval outcomes,
bounded v3 framing, PTY output pacing, TCP/PTTY/local-input polling, established
disconnect, and opt-in reconnect. Ctrl+C remains PTY data while Ctrl+\\ is the
local escape. Fake TCP peers cover fallback, denial, malformed handshakes,
bidirectional v3 traffic, disconnect, and reconnect. A subprocess test sends
SIGTERM during approval and verifies both termios and descriptor flags are
restored.

The fourth slice ports all four diagnostics suites and the deterministic JSONL
contract. It validates response identity, PRESENTED/READY order, uint32 wrap,
cadence sequence ranges, coalescing metadata, rejection, timeout, disconnect,
and STOP cleanup. A fake X4 completes the full smoke flow and a coalesced
cadence group.

The Rust host declares Rust 1.80 as its minimum, uses `nix` 0.31.3 for
Darwin/Linux Unix wrappers, and passes formatting, 43 Rust tests, strict Clippy,
an optimized build, and a real PTY smoke. Before deletion, all 39 legacy tests
also passed as the final behavior-oracle check. The user confirmed the complete
non-daemon foreground and diagnostics matrix on macOS and Linux. Exact Linux
environment metadata was not copied into the worktree; that limitation is
recorded in `host-rs/HOST_MATRIX.md`. The tracked Python/uv package was removed
after integration paths and fixtures moved under `host-rs/`.

## Architecture choice

Use one small synchronous Unix event loop around `poll`/equivalent. The workload
has a TCP socket, PTY master, and local terminal descriptor; a general async
runtime adds complexity without improving panel latency. Isolate these modules:

```text
cli | discovery | protocol | transport | pty | terminal_guard | diagnostics
```

Use safe crates for CLI, signals, and Unix APIs where they materially reduce
unsafe code, but verify current upstream versions and macOS/Linux support at
implementation time. Keep all direct `unsafe` calls in the PTY/terminal module,
document invariants, and test cleanup on every error path. Do not add a serial
dependency unless USB support is intentionally retained and tested.

The Rust binary has two long-lived operating forms sharing the same modules:

```text
knietty connect ...   # foreground PTY bridge; current interactive behavior
knietty daemon ...    # persistent discovery/session supervisor
```

Keep existing compatibility aliases where practical so the cutover does not
force users to relearn the working CLI.

## Persistent discovery and multiplicity

Discovery must converge regardless of startup order. The daemon sends bounded
periodic discovery probes, while an available X4 continues mDNS advertisement
and may send a rate-limited availability announcement at boot/network join.
Either side starting later therefore shortens discovery without creating a new
connection direction: the host still initiates TCP and the X4 still owns the
physical approval gate.

Give every daemon installation a persistent random host ID and human-readable
name; retain the X4's existing device ID. Before authenticated pairing exists,
these IDs are routing hints only and must not be treated as security principals.
Milestone 06 binds them to TLS identities.

Use conservative multi-peer rules:

- Unpaired devices are listed but never auto-claimed by every daemon.
- An X4 accepts only one pending approval or active session and reports busy to
  other proposals.
- Automatic connection requires an explicit device-to-host preference. With no
  preference, the CLI requires `--device ID` or physical selection.
- One daemon may supervise multiple explicitly assigned X4s, each with its own
  PTY/tmux session such as `knietty-<device-id>`.
- Two daemons competing for one assigned X4 do not race indefinitely: the
  preferred host may connect, non-preferred hosts back off with visible status,
  and all retry loops use jittered bounded intervals.
- A manual foreground connection can either fail clearly on a daemon-owned
  device or request an explicit takeover; it never silently steals a session.

## Behavioral contract

- Same CLI defaults, 80 x 24 PTY geometry, `TERM=vt100`, tmux preference, shell
  fallback, client-name rules, discovery ambiguity rejection, and explicit IP.
- Local Ctrl+C goes to the PTY child process group; it never kills the bridge.
- Local Ctrl+\\ restores terminal state and exits, including retry waits.
- Established disconnect exits by default; `--reconnect` rediscovers and keeps
  the tmux session model.
- Daemon mode continuously discovers configured devices, reconnects only those
  assigned to this host ID, and does no PTY work while idle.
- Child exit, signal, panic, I/O error, and protocol error restore termios and
  reap the child. Add a panic hook only as a final restoration fallback.
- Diagnostics output preserves the frozen golden-session byte/schema contract.

## Implementation order

1. Create a separate Rust package under `host-rs/` while retaining the oracle.
2. Port v3 codec/golden vectors and discovery.
3. Port PTY spawn/window/environment and terminal RAII guard.
4. Port bridge/select loop, signals, child groups, and reconnect.
5. Port diagnostics and deterministic JSONL serialization.
6. Check the frozen cross-language fixture and deterministic JSONL contract.
7. Run the same fake-device integration scenarios before removing the oracle.
8. Switch integration templates only after real Linux and macOS parity.

The Codex/TUI parser compatibility work is intentionally deferred until this
Rust milestone is complete. Its locked scope is recorded in
[`TERMINAL_COMPATIBILITY.md`](TERMINAL_COMPATIBILITY.md).

## Verification gate

- `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, and tests.
- The legacy 39-test oracle passes once immediately before removal.
- Leak/file-descriptor checks where available on each OS.
- Forced denial, malformed frame, socket reset, PTY child exit, SIGTERM,
  Ctrl+C, Ctrl+\\, and reconnect restore the local terminal.
- macOS and Linux each run discovery, shell, tmux, diagnostics smoke, and
  disconnect. Record OS/version separately.

## Complete when

Rust passes the documented behavior matrix on both target operating systems;
fixtures and integration paths are self-contained under `host-rs`; the
superseded Python package is removed. Daemon mode remains separately backlogged.
