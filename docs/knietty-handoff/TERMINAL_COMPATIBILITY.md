# Post-Rust terminal and Codex compatibility

## Scheduling

Begin this work only after Milestone 05 reaches Rust host parity. It is a
firmware terminal-compatibility pass, not part of the host-language migration.
Keep the validated display profile and scheduler behavior fixed while testing
parser changes.

## Objective

Make interactive Codex and comparable TUIs render coherently at 80 x 24:
correct scroll behavior, stable cursor placement, no control-string leakage,
and no literal `q` or replacement glyphs for the observed session corpus.
This does not require full xterm or full-Unicode compatibility.

## Immediate host-side profile

Use a dedicated Codex profile to reduce unnecessary E Ink churn:

```toml
[tui]
animations = false
alternate_screen = "never"
terminal_title = []
notifications = false
```

Keep this user opt-in. Do not silently rewrite a user's Codex configuration.

## Implementation order

1. **Implemented:** the Rust host's explicit `--capture-output PATH` records
   host-to-X4 PTY bytes in a new mode-`0600` file, refuses overwrite, and stops
   at a configurable 8 MiB bound. Capture a representative Codex session,
   replay it into native terminal tests, and retain only redacted/synthetic
   golden streams in the repository. PTY echo means captured output may include
   typed commands even though the input stream itself is not recorded.
2. **Hardware-tested:** consume OSC, DCS, APC, and PM strings through BEL or ST
   without allowing their payloads onto the screen. The first Codex capture
   confirmed OSC title payloads caused the visible `0;…` corruption.
3. Implement G0/G1 designation and DEC Special Graphics mapping when a capture
   or target application requires it. The first capture did not use it; its
   literal `q` came from cursor-style CSI parsing instead.
4. **Partially hardware-tested:** add `ESC 7/8`, `CSI s/u`, scroll margins,
   index/reverse-index, and explicit scroll up/down. The capture used scroll
   margins, `ESC M`, and `CSI S`; these are implemented. Origin mode and any
   additional cursor forms remain driven by future captures.
5. Add insert/delete/erase character and line operations when observed. Explicit
   scroll-up/down from the first capture is already covered by step 4.
6. Reply safely to cursor-position/device-status/device-attribute queries.
   Ignore optional modes only when doing so cannot make the application wait.
7. If the capture contains synchronized-output mode boundaries, use them as
   E Ink presentation commit points. Otherwise keep bounded latest-state burst
   coalescing rather than presenting animation intermediates.
8. Implement a bounded alternate screen for general TUI compatibility after
   the Codex `alternate_screen = "never"` path is correct.
9. Record missing code points and add the observed box, block, arrow, bullet,
   and punctuation glyphs with correct one/two-cell width. Defer emoji,
   arbitrary combining behavior, and comprehensive Unicode shaping.

## First capture findings

The private capture is not committed. A control-only inventory found the
current corruption mechanisms:

- OSC terminal-title strings were treated as printable bytes, producing the
  observed `0;hostname…` text.
- `CSI 0 SP q` cursor-style commands were abandoned at the intermediate space,
  leaving their final `q` printable. This—not DEC line drawing—caused the
  observed status-line `q` in this session.
- Codex relies on DEC scrolling margins, reverse index, and explicit scroll-up;
  ignoring them left the latest response outside the visible model.
- The capture contained no G0/G1 charset designation, so DEC Special Graphics
  mapping is not required for this specific failure. Designations are now
  consumed safely but mapping remains pending until observed or separately
  tested.
- Synchronized-output mode is frequent and is a promising E Ink commit hint,
  but it is deliberately deferred until parser correctness passes on hardware.

## First hardware result

The parser-only W100 experience image was exercised with Codex on the available
X4. Transcript scrolling rendered correctly and the leaked cursor-style `q`
disappeared. The remaining visible `?` was confirmed to be Codex's warning icon,
so it is expected application content rather than terminal corruption. The
explicit `SESSION_END` follow-up image and updated Rust host subsequently exited
gracefully on the same X4/macOS setup, closing that regression gate.

## Acceptance gate

- A recorded synthetic Codex session replays deterministically in native tests.
- Thinking, tool output, completion, and transcript scrolling preserve cursor
  position and existing text.
- No control payload becomes printable text.
- No literal DEC line-drawing letters or replacement glyphs remain for the
  captured corpus.
- Ctrl+C, terminal exit, reconnect, and the validated E Ink update scheduler do
  not regress.
- Physical validation identifies the exact firmware artifact and host OS; one
  host platform does not establish Linux/macOS parity.
