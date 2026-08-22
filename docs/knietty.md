# knietty terminal mode

knietty is an opt-in CrossPoint build plus a user-space host bridge. It retains
the reader firmware and activity lifecycle, uses CrossPoint's saved Wi-Fi
configuration, and renders a bounded character-cell screen into the existing
E Ink framebuffer. USB CDC remains available as a legacy transport, but no CDC
device was exposed by the tested China-locked X4.

Nothing in this document is evidence of physical X4 validation. Check
`TTY_PROGRESS.md` for the exact tested state before flashing.

Future work is locked into the ordered
[knietty implementation handoff](knietty-handoff/README.md). Each milestone has
its own scope, source anchors, automated checks, physical test gate, rollback
conditions, and definition of completion. Agents should not skip its diagnostic
baseline or combine independent SSD1677 experiments in one firmware image.

## Firmware architecture

Build environment `knietty` adds `KNIETTY_ENABLED` and
`KNIETTY_STABLE_POWER`, and deliberately omits `ENABLE_SERIAL_LOG`. The stable
power flag retains CrossPoint 1.5.0's deep-sleep resume semantics and disables
the newer development branch's experimental light-sleep paths for this build.
Terminal mode itself prevents auto-sleep and owns the Power button; after
leaving Terminal, normal CrossPoint sleep behavior resumes.

The Home menu opens `TerminalActivity`, which selects a saved network and then
switches the renderer to native 800 x 480 landscape. It owns:

- a fixed 80 x 24 screen of four-byte Unicode cells (7,680 bytes per model);
- a small VT100-style parser with bounded parameters and incremental UTF-8;
- a generated 1,001-glyph Spleen 8 x 16 bitmap table stored in flash, with
  build-selectable Terminus and GNU Unifont alternatives;
- 10 x 18 cells with a four-pixel left bezel inset; four cells surrender one
  gutter pixel apiece so all 80 columns still fit in the 800-pixel panel;
- dirty row spans and a render snapshot so network RX continues while the
  E Ink waveform runs; before an ordinary refresh, final glyph, attribute, and
  cursor state are compared with that snapshot so clear-and-identical-repaint
  traffic from full-screen TUIs does not drive unchanged edge cells or rows;
- Wi-Fi discovery/approval/stream transport and logical CrossPoint button
  input.

Protocol v3 terminal, approval, and diagnostics traffic is wrapped in mutually
authenticated TLS 1.3. A first pair requires matching the six-digit code shown
by the Rust host and X4 before pressing Confirm. Later terminal reconnects from
that pinned host are automatic; diagnostics continues to require physical
approval every time. Plaintext UDP discovery carries only presence metadata and
advertises `tls=required`.

RX is drained continuously while the render task waits for the panel. Parser
mutations occur behind a short model lock; rendering copies a stable snapshot
and releases that lock before drawing or refreshing. Matching hosts negotiate
`burst1`: PTY output is framed at up to 512 bytes, drained at the configured
pacing deadline, and followed by a boundary after 24 ms of PTY quiet. Firmware
presents that complete logical burst, with an 80 ms fail-safe after the latest
payload if the marker does not arrive. Older hosts retain the 8 ms quiet / 20 ms maximum firmware batching
path. Only the final changed column span of each dirty row is redrawn. Named hardware
diagnostic activations deliberately bypass this pruning so their requested
geometry and timing remain reproducible. Normal updates use
the X4 SSD1677 byte-aligned differential-window path when the temporary transfer
is at most 8 KiB; larger or unsupported regions safely fall back to the resident
whole framebuffer.

The adaptive `knietty` profiles select terminal-only SSD1677 differential
waveforms with unchanged black/white pixels idle. The one-frame profile is the
maximum-speed tradeoff and has measured weak contrast and accumulating ghosting.
The isolated quality profile retains the specified 20 MHz SPI clock and extends
the same transition drive to a nominal 100 ms; its actual BUSY duration and
optical quality must be measured on hardware. Two further A/B profiles isolate
its remaining problems: `knietty_adaptive_100ms_nosettle` removes only the
automatic quiet-time settle, while `knietty_adaptive_100ms_sustain` keeps that
scheduler and adds a short opposite-polarity sustain/restore pair for unchanged
pixels within the same nominal 100 ms. After their physical A/B, the combined
`knietty_adaptive_100ms_sustain_nosettle` profile also limits Terminal battery
status sampling to once per minute so noisy X4 ADC readings cannot continuously
dirty the full-width header. The profile is restored to the
panel default before Terminal exits; FULL and HALF refreshes are never replaced,
and no periodic clean interrupts an active session. The controller format and
RAM/LUT mapping follow the
[SSD1677 data sheet](https://files.waveshare.com/upload/2/2a/SSD1677_1.0.pdf).
A clean HALF refresh is still used on entry and after host disconnect.

Supported input is BMP UTF-8 with a replacement glyph for invalid/non-BMP
input, LF, CR, backspace, tab, BEL (ignored), delayed line wrap, scroll, cursor
movement, screen/line erase, cursor visibility, and SGR
normal/bold/inverse/underline. ANSI colors are accepted and ignored. Spleen
adds Latin, Greek, Cyrillic, box drawing, block elements, Braille, and a small
Powerline subset; this is a compact terminal font, not a complete Nerd Font.
The cursor is a static one-pixel underline rather than an inverse block, keeping
the glyph beneath it readable and minimizing E Ink pigment movement.

Controls are:

| Logical button | PTY input |
| --- | --- |
| Up / Down / Right / Left | `ESC [ A/B/C/D` |
| Confirm | carriage return |
| Back | Escape |
| Confirm held at least 1 second | Ctrl+C |
| Back held at least 1 second | toggle black/white polarity |
| Power, then Power within 3 seconds | leave terminal mode |

The logical mapping honors CrossPoint's configured front-button mapping; no
physical GPIO identifiers are embedded in terminal code.

While waiting for a host, the screen shows the discovered name/address and a
compact control reference. Left or Right switches to on-device refresh timing:
total update, waveform wait, transfer, render, minimum/average/maximum, and
window/fallback counts. The approval view shows mapped Confirm=Accept and
Back=Deny hints; those hints are cleared once a host connects.

## Terminal fonts

Open [the generated terminal font gallery](terminal-font-gallery.html) in a
browser. It renders the exact 1-bit arrays compiled into firmware, including
shell punctuation, box/block drawing, arrows, Powerline symbols, Greek, and
Cyrillic. Missing characters use the same `?` fallback as the device.

Profiles are:

| Font | PlatformIO environment | Compiled glyphs |
| --- | --- | ---: |
| Spleen 8 x 16 | `knietty` | 1,001 |
| Terminus 8 x 16 | `knietty_terminus` | 937 |
| GNU Unifont 8 x 16 | `knietty_unifont` | 978 |

These remain single-cell 8-pixel fonts. A full Nerd Font is not suitable yet:
many symbols are double-width and the terminal model does not implement
`wcwidth`, combining, or shaping. Font sources, versions, licenses, and the
generated-table provenance are in
[`TerminalFonts-LICENSES.md`](../src/terminal/TerminalFonts-LICENSES.md).

## Build

This checkout pins pioarduino PlatformIO Core 6.1.19 in a local uv-managed
environment. The reproducible development commands are:

```sh
env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  .venv/bin/uv pip install --python .venv/bin/python --upgrade \
  https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip

env UV_CACHE_DIR=/private/tmp/knietty-uv-cache \
  PLATFORMIO_CORE_DIR=/private/tmp/knietty-platformio \
  .venv/bin/pio run -e knietty

# Optional exact-font variants
.venv/bin/pio run -e knietty_terminus
.venv/bin/pio run -e knietty_unifont

# Experimental W100 attribution variants; physical A/B required
.venv/bin/pio run -e knietty_adaptive_100ms_nosettle
.venv/bin/pio run -e knietty_adaptive_100ms_sustain
.venv/bin/pio run -e knietty_adaptive_100ms_sustain_nosettle
```

Outputs are under `.pio/build/<environment>/firmware.bin`. Rebuild the gallery
after changing a generated table with:

```sh
python3 scripts/generate_terminal_font_gallery.py
```

Knietty application releases use one project version from `[knietty]` in
`platformio.ini`. Their SD artifact and on-device build identity are both named
`knietty-x.y.z`; ordinary CrossPoint environments keep the upstream development
version format.

## Locked-unit update and recovery

The tested X4 is China-locked and exposes no application CDC device. Do not use
PlatformIO upload, esptool, or alter its partition table, bootloader, secure-boot
state, or eFuses. Keep the known-good CrossPoint 1.4.1 application image and a
copy of every tested knietty image outside the build directory.

CrossPoint's normal network OTA has physically restored official 1.5.0 without
the Unlocker, establishing the recovery route for this unit. From that clean
base, an SD-menu installation of the SD-safe knietty image also completed and
booted. Use the same in-application SD update path for subsequent knietty
application images, and retain the official OTA route as recovery. No partition
changes are part of knietty.

Knietty serializes SD update progress with the display because both devices
share SPI on X3/X4. Progress paints are synchronous and coarse (10% steps), so
the next SD read cannot overlap an E Ink transfer. Saved-network entry first
tries the last credential directly; if that early attempt fails before a scan,
it gets one post-scan retry before the ordinary network list is shown.

The exact current artifact name, checksum, observed updater behavior, and next
hardware test are recorded in `TTY_PROGRESS.md`; that file takes precedence
over generic flashing instructions.

## Host bridge

Build or install the Rust host (Rust 1.80 or newer):

```sh
cargo install --locked --path host-rs
knietty --host auto
```

The default command is `tmux new-session -A -s knietty` when tmux is installed,
otherwise `$SHELL`. The PTY receives `TERM=vt100`, `COLUMNS=80`, `LINES=24`, and
the corresponding `TIOCSWINSZ` geometry. Ctrl+C read from the local terminal is
written to the PTY and signals only the PTY child process group. Ctrl+\\ exits
the bridge even during retry waits. An established disconnect exits by default;
pass `--reconnect` for systemd/launchd operation so discovery resumes and the
tmux session can reattach.

Current protocol-v3 firmware sends an explicit `SESSION_END` frame before a
deliberate Terminal exit, so the foreground host does not depend on a TCP FIN
surviving the X4's immediate Wi-Fi teardown. The Rust host also uses short TCP
keepalive probes to bound stale sessions after abrupt power or WLAN loss.

While that foreground bridge is connected, another shell can issue bounded
display controls over the bridge's private local Unix socket:

```sh
knietty display status
knietty display metrics --json
knietty display clean
knietty display polarity inverted
knietty display polarity normal
```

Mutation commands are quiet by default. In particular, `display clean` lets
the invoking shell repaint its prompt, waits for 500 ms without PTY output, and
then performs the clean so command output does not immediately smudge the
panel. For synchronous measurement use `knietty display clean --wait --json`.
`display status` and `display metrics` always return JSON; add `--json` to a
polarity command when its refresh telemetry is required. Metrics reads the
current aggregate counters without refreshing or dirtying the panel.

`status` reports the active firmware/display profile, SPI clock, feature flags,
battery/RSSI, terminal geometry, heap, and build revisions. `metrics` returns
the update/window/fallback/settle/clean counts, last and aggregate timings,
last region, heap snapshot, and bounded host/device RX, frame, burst, snapshot,
timeout, and async-tail counters. `clean` performs the
same safe HALF refresh already used by Terminal; polarity is explicit rather
than a toggle. Refreshing commands return only after READY telemetry. These
commands require protocol v3 and an already-authenticated active bridge. The
local socket is user-owned and mode `0600`; the LAN session is TLS 1.3.

The host stores a persistent private P-256 identity and per-device pins with
owner-only permissions. The X4 stores its identity and up to four pinned hosts
in CRC-protected NVS records. Hold Confirm on the waiting screen, then press it
again within five seconds, to forget all X4-side hosts. Discovery itself is not
authenticated and contains no terminal data. The current X4 has no secure
element, so physical flash extraction is outside the pairing threat model.

Wi-Fi automatic discovery uses a bounded UDP broadcast probe; the X4 also
advertises `_knietty._tcp.local`. It rejects ties. Use an explicit address when
the network contains more than one terminal:

```sh
knietty --list-devices
knietty --host 192.168.1.42
```

Run directly from the repository without installing when developing:

```sh
cargo run --release --manifest-path host-rs/Cargo.toml -- --list-devices

cargo run --manifest-path host-rs/Cargo.toml -- \
  pty-smoke --command 'printf "%s:%s:%s\\n" "$TERM" "$COLUMNS" "$LINES"'

cargo run --release --manifest-path host-rs/Cargo.toml -- \
  --host auto --verbose
```

The Rust bridge and diagnostics matrix were user-confirmed on both macOS and
Linux. See `host-rs/HOST_MATRIX.md` for the procedure and evidence caveats.
The tested China-locked X4 did not expose application USB CDC, so this host is
Wi-Fi-only.

## Linux user integration

Install the host command for the current user:

```sh
cargo install --locked --path host-rs
```

Inspect the connected device before creating a udev rule:

```sh
lsusb
dmesg --follow
ls -l /dev/ttyACM*
udevadm info /dev/ttyACM0
```

USB CDC is not needed for the current Wi-Fi bridge. For future hardware that
does expose CDC, copy `host-rs/integration/linux/99-knietty.rules.example`, replace every placeholder
with observed values, keep only one rule, and install it as
`/etc/udev/rules.d/99-knietty.rules`. Reload rules through the distribution's
normal administrative workflow. The rule adds `/dev/knietty` and `uaccess`; it
does not start a root daemon.

For a persistent per-user bridge:

```sh
mkdir -p ~/.config/systemd/user
cp host-rs/integration/linux/knietty.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now knietty.service
```

This separation is intentional: udev provides access/stable naming, while the
already-running user service performs discovery and owns the user's tmux session.

## macOS user integration

Inspect the connected device first:

```sh
ls /dev/cu.usb*
ls /dev/tty.usb*
system_profiler SPUSBDataType
ioreg -p IOUSB -l -w 0
```

Install the command with `cargo install --locked --path host-rs`. Copy
`host-rs/integration/macos/dev.knietty.host.plist.in` to
`~/Library/LaunchAgents/dev.knietty.host.plist`, replacing
`__KNIETTY_EXECUTABLE__` with the absolute path reported by `command -v knietty`
and `__HOME__` with the absolute home directory. launchd does not expand shell
variables or `~` in plist strings.

Validate and load it as the current user:

```sh
plutil -lint ~/Library/LaunchAgents/dev.knietty.host.plist
launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/dev.knietty.host.plist
```

No root LaunchDaemon, kernel extension, or DriverKit component is required.

## Validation matrix

Run the complete host software gate on each OS and record the OS/version in
`TTY_PROGRESS.md`:

```sh
./host-rs/scripts/check.sh
```

Hardware validation must separately record first-prompt latency, output-to-panel
latency, button-to-PTY latency, actual refresh durations, ghosting, runtime free
heap, CPU/battery behavior, unplug/replug behavior, and tmux survival. Linux
results cannot be used to claim macOS parity or vice versa.
