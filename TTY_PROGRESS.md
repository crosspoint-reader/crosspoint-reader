# Project: knietty

## Project description

**knietty turns an XTEINK X4 running CrossPoint into a low-latency wireless
terminal for shell and tmux sessions.** It discovers a host over the local
network, renders a compact VT-style terminal optimized for E Ink, and relays
device input back to the host without replacing CrossPoint's reader experience.

Short description: **A wireless TTY for your E Ink reader.**

## Current milestone

Release validation is complete for the clean `knietty-0.1.2` candidate rebased
onto current CrossPoint `develop` (`69dd947f`) and FreeInk `main`. The user
physically passed the available X4/macOS gate for automatic saved-Wi-Fi
reconnect, connection approval and TLS, typing and btop, display commands,
graceful exit, returning to reader operation, and sleep/wake outside Terminal.
The rebased tree also passes the complete Rust host matrix, 177/177 native
tests, and the `knietty_async_window` firmware build. The clean implementation
checkpoints are parent `5135f049` and FreeInk `0b9b49f`. They are published as
dependent drafts in FreeInk PR #49 and CrossPoint PR #3158. Upstream CI on
software head `f2497a77` passes formatting, cppcheck, all 177 native tests, and
the Default, Sticky, X4 Pro, and PaperMono firmware builds.

Milestone 07 is complete with a negative optimization result. Milestone 08 is
complete on the ordinary no-Mode-2
`knietty_adaptive_100ms_sustain_nosettle` profile. Terminal already receives
and parses TCP on the main task while the separate render task waits on panel
BUSY, and `TerminalRenderGate` already coalesces an arbitrary in-flight burst
into one newest replay. The first Milestone 08 candidate bulked framed TLS reads
and raised the nominal host pacing ceiling from 64 to 256 KiB/s. Its hardware
A/B showed no visible difference, but that test did not exercise the host-loop
wake defect found below, so it no longer excludes host delivery cadence.

The next isolated candidate combines sparse dirty row spans into one SSD1677
activation and overlaps BUSY with composing the newest terminal state. It uses
one lazily allocated 8 KiB immutable staging buffer, never a second 48 KiB
framebuffer, and retains the proven blocking path for oversized/full-screen,
status, diagnostics, clean, settle, inverted, and first-render work. The user
physically validated coherent multi-region updates with no regression, but saw
no typing or btop improvement. btop remains chunked and its clock advances every
two to three seconds even though the host configuration is confirmed at
`update_ms = 500`. Live status after exercising the path reported 53,416 bytes
free heap and a 44,588-byte minimum. Phase B is therefore a negative performance
result with a successful correctness/memory gate.

The metrics-enabled follow-up is physically reachable through
`knietty display metrics --json`. A live btop sample showed about 5.3 panel
updates per second, 1,125 window updates versus only eight full-frame fallbacks,
and about 187 ms average display time. The panel was therefore not waiting two
to three seconds: the host was delivering each large TUI repaint in chunks.

Inspection then found the matching root cause in the Rust bridge. Protocol v3
frames carry at most 512 PTY bytes. After writing one frame, the bridge set an
approximately 2 ms pacing deadline, but when that frame drained it stopped
polling the PTY and slept for the generic 100 ms event-loop interval. Effective
large-burst throughput was therefore only about 5 KiB/s; a 10--15 KiB btop
paint naturally arrived over two to three seconds. The `knietty-0.1.0`
candidate wakes at the actual pacing deadline, advertises an optional `burst1`
capability, sends one boundary after 24 ms of PTY quiet, and lets firmware
snapshot a complete logical burst before refreshing. An 80 ms firmware timeout
measured from the latest payload prevents a missing marker from freezing
display output without splitting a long active burst. Older v3 hosts and
firmware retain their previous behavior. The user physically confirmed that
btop now loads and updates quickly on this candidate. Live pipeline counters
show byte-perfect delivery and the intended two completed bursts per second.
The main cadence gate passes. Review of the apparent 80 ms safety-timeout
failures found a timestamp race rather than a tuning problem: `render()`
sampled its start time before taking `modelMutex`, while network RX could store
a newer `lastQueuedAt` before the render acquired that mutex. Unsigned
subtraction then underflowed and falsely declared a timeout. The
`knietty-0.1.1` candidate samples `millis()` after loading `lastQueuedAt` and
keeps the proven 80 ms fail-safe unchanged. The user SD-flashed this candidate,
connected, and ran btop. A one-minute live sample ended with byte- and
boundary-perfect host/device counters, approximately 2.55 window updates per
second, no new full-frame fallbacks, stable minimum heap, eight timeouts, and
six async tails. This cuts both residual rates by roughly three quarters from
the 0.1.0 gate without changing latency. Exact captures are in
`results/knietty-0.1.1-btop-live.md`.

The user subsequently confirmed the saved network reconnects automatically
without manual confirmation. The `knietty-0.1.1` firmware, Rust host transport,
terminal controls, deliberate exit, reconnect, and reader sleep/wake are now the
working Milestone 08 checkpoint. SD progress remains queued for observation by
the next update executed from this firmware; it is not a Milestone 08 blocker.

The isolated TLS/pairing checkpoint is `edf80251`. The first SSD1677
Mode-2 RAM ping-pong candidate used
parent `4f105b1f` and FreeInk `8ff8d51` behind the
`knietty_mode2_pingpong` environment. Its bounded smoke transport passed, but
the visual correctness gate failed: ordinary keystrokes could restore portions
of the diagnostics waiting page and frames alternated inside dirty windows. This
rejects zero-copy window ping-pong. The retained capture and analysis are
`results/mode2-pingpong-4f105b1f.*`; no later suites were run.

The second candidate uses FreeInk `9406d39`. After every Mode-2 activation it
synchronizes the just-presented full frame or dirty window into the newly
inactive bank through one 0x24 write. Its state machine exposes the unsynchronized
interval and forces absolute recovery if interrupted. The 174-test native
suite, parent `61e61088` firmware build, and ordinary no-flag firmware
regression pass. The replacement image was frozen for its physical smoke gate.

That synchronized image then passed the physical visual-correctness smoke gate:
the waiting-page leakage and alternating old/new windows were gone, and the user
reported that it looked very good. Telemetry accepted all 14 commands and
completed all 13 refreshes without a failed event. It is not promoted because
true-window median remained effectively flat at 102.5 ms versus 102.1 ms, while
fast full-frame fallback regressed from 207.5 to 349.9 ms. Mode 2 provides no
optical improvement by itself and recovered no timing budget for clarity. Its
latency/cadence/burst suites were therefore intentionally skipped. Milestone 08
will use the ordinary legacy baseline path.

The Wi-Fi proof of concept, 80 x 24 stabilization image, and Terminus turbo
image have run on the available China-locked X4. The user confirmed the Home
icon, normal sleep/wake, earlier host disconnect behavior, UTF-8 box drawing, btop,
two-press exit, Terminus rendering, waiting tips, and timing page.

Milestone 01 is physically validated. Terminal now
temporarily disables the renderer's fading fix, preserves the incoming refresh
profile/orientation, and restores all of that state on exit without changing the
saved reader setting. A race-safe render gate also preserves one coalesced
follow-up paint when network state changes during an in-flight E Ink refresh;
the user confirmed the formerly invisible host approval prompt now renders.

Milestones 02 and 03 are physically validated on the available X4. Protocol v3
carried the normal terminal, forced v2 remained compatible, and the distinct
diagnostics approval, denial, abort, cleanup, JSONL output, and post-exit
sleep/wake checks passed. The bounded smoke capture is retained at
`results/gate-b-smoke.jsonl`.

Milestone 04 is complete. The matched safe 20 MHz and adaptive 40 MHz smoke,
latency, cadence, and burst captures passed on the available X4 with no rejected
commands. Their raw JSONL, hashes, conditions, caveats, and separated
window/fallback summaries are frozen as `results/baseline-v1.md`.

Milestone 05, the Rust host migration, is complete. `host-rs/` is now the sole
knietty host package and owns v3/v2/v1 negotiation, discovery, PTY/process-group
management, terminal restoration, foreground reconnect, all four approved
diagnostics suites, deterministic JSONL, and the frozen protocol fixture. It
passes formatting, strict Clippy, 43 Rust tests, an optimized build, and a live
PTY smoke. The final pre-removal oracle run passed all 39 legacy tests.

The user confirmed the complete non-daemon foreground/diagnostics matrix on
macOS and Linux, including discovery, explicit addressing, approval/denial,
shell/tmux, Ctrl+C, Ctrl+\\, disconnect/reconnect, all diagnostic suites,
interrupt cleanup, and post-diagnostics sleep/wake. The exact Linux
distribution/toolchain record and most raw captures were not copied into this
worktree; a 41-line macOS smoke capture is retained under
`results/rust-host-matrix/`. Integration templates and fixtures now live under
`host-rs/`, and the superseded Python/uv host source, package, lock, and tests
were removed together. Persistent daemon supervision remains backlogged.

Milestone 09's first controlled waveform-quality experiment has passed its
bounded smoke gate on the available X4. It keeps the SSD1677 write clock at its
specified 20 MHz, keeps the one-frame profile's directional transitions and X4
analog values, and changes only the volatile phase-A duration from one to twenty
5 ms frames. The measured window waveform median is 100.078 ms. Ordinary typing
is much better, but full-screen TUI repaint cadence and accumulating gray grain
remain unresolved.

The first software follow-up was physically A/B tested. Ordinary
terminal rendering now prunes dirty edge cells and complete rows whose final
glyph, attributes, and cursor overlay match the last presented snapshot. It is
allocation-free and leaves named diagnostics unpruned. This targets TUI
clear-and-identical-repaint traffic without changing the 100 ms waveform, SPI
clock, batching deadlines, or driver behavior. The user reported that sustained
input still made the screen progressively grainy gray, occasional maintenance
passes only partly cleared it, and btop still lagged and accumulated residue.
The rendering hypothesis is therefore not the primary cause.

The scheduler and sustain candidates are now physically tested. No-settle still
showed a btop clock update only every two to three seconds and progressive gray
grain. Toggling inversion and back a few times temporarily cleared the grain,
which later returned. That run also exposed a rapid 68--78% battery oscillation.
Source inspection found that the plain X4 ADC battery path was sampled on every
Terminal loop and every one-percent change dirtied the full-width header. When
coalesced with terminal output, that expands the bounding rectangle and can
force the 8 KiB window path into a nearly full-frame fallback. Therefore the
no-settle cadence result is confounded by real status repaint traffic, not by
the CPU cost of ADC polling alone.

Sustain1 delivered near-perfect ordinary typing with only acceptable cursor
ghosting, but btop accumulated severe grain. An invert-and-return cycle again
cleared it temporarily. This rejects the particular equal-time 5 ms/5 ms
unchanged-pixel pulse as a general TUI-quality solution, while retaining its
typing result as useful. A combined candidate now keeps Sustain1, disables the
automatic settle, and samples the Terminal battery status only once per minute.
It is software-tested and awaits the next physical btop comparison.

The next pickup is the saved terminal/Codex compatibility pass in
`docs/knietty-handoff/TERMINAL_COMPATIBILITY.md`, followed by TLS pairing and
then independently measured SSD1677 experiments. BLE keyboard work is
explicitly backlogged until the Wi-Fi terminal, Rust host, encrypted transport,
and display scheduler are stable.

The first compatibility slice is now implemented in the Rust host. Explicit
`--capture-output PATH` records the exact host-to-X4 PTY byte stream into a new
mode-`0600` file, refuses overwrite, and aborts cleanly at an 8 MiB default
bound. The repository ignores `captures/` because screen output and PTY-echoed
commands may contain sensitive data. Native tests cover permissions, bounds,
overwrite refusal, and capture through the v3 bridge. A representative,
privacy-reviewed Codex trace is the next required input to parser replay tests.

The first Codex trace was captured and inventoried locally without committing
its screen contents. It identified three concrete parser failures: OSC title
payloads leaked as the visible `0;…` prefix; `CSI 0 SP q` cursor-style commands
were abandoned at their intermediate byte and leaked the final `q`; and Codex's
scroll margins, reverse-index, and explicit scroll-up operations were ignored,
leaving newer response rows outside the correct visible model. The firmware now
consumes OSC/DCS/APC/PM strings through BEL/ST, consumes unsupported CSI
private/intermediate forms atomically, saves/restores cursor state, and applies
line feed, auto-wrap, reverse index, and explicit up/down scrolling only inside
the active DEC margins. Four synthetic capture-derived tests raise the native
suite to 166/166. Synchronized-output presentation remains a later, isolated
latency change. The user then physically validated the parser-only experience
image on the available X4: Codex scrolling is good and the phantom cursor-style
`q` is gone. A remaining `?` was identified as Codex's intentional warning icon,
not parser leakage or a missing-glyph regression.

A later physical session exposed an intermittent clean-exit regression: the X4
returned to CrossPoint but the host TCP stream could remain established because
the firmware stopped the socket immediately before disabling Wi-Fi. Protocol v3
now defines an empty mandatory `SESSION_END` frame. Firmware sends it with a
25 ms WLAN grace interval before teardown, and the Rust bridge treats it as a
clean disconnect even if the TCP peer deliberately remains open. The Rust host
also enables a roughly six-second TCP keepalive fallback for abrupt power/WLAN
loss. Both profiles build and all software gates pass. The user physically
validated the combined experience image and updated Rust host on macOS: exiting
Terminal closed the host gracefully and returned the X4 to CrossPoint.

The runtime-control product slice is physically validated. A connected
protocol-v3 foreground bridge exposes a per-device Unix socket under a private
per-user runtime directory. `knietty display status`, `display metrics`,
`display clean`, and explicit `display polarity normal|inverted` reuse the approved TCP
session. Firmware accepts only session-info, clean, and polarity in Terminal
mode; diagnostics reset/pattern/stop and all raw SSD1677 controls remain
inaccessible. Metrics is a fixed read-only snapshot of the same aggregate
values rendered on the device's Refresh diagnostics page and does not trigger
or dirty a display update. The original payload was 84 bytes; the current
108-byte payload appends bounded host/device pipeline counters and the host
decoder accepts both versions. The user confirmed status, clean, and polarity on the current
combined W100 experience image. That check exposed excessive mutation JSON and
post-clean prompt repaint; the host-only follow-up makes mutations quiet and
defers ordinary clean until the PTY has been silent for 500 ms. Synchronous
READY telemetry remains available through `display clean --wait --json`.

The delayed-clean correction is committed as `c07520d4`. Milestone 06's TLS
candidate is now physically working on the available X4 and macOS host. UDP
discovery remains plaintext and advertises `tls=required`; the accepted TCP
socket is wrapped in mutually authenticated TLS 1.3 before the v3 greeting.
The X4 generates and persists a unique P-256 identity plus a fixed four-host
pin table in CRC-protected NVS records. The Rust host persists its own P-256
identity and per-device certificate pins atomically with owner-only
permissions. The user confirmed the first-pair approval, unattended trusted
reconnect, terminal traffic without observable TLS lag, runtime display
commands, exit, sleep/wake, diagnostics approval, and X4 forget-all behavior.
Status reported 63,828 bytes free heap and a 45,800-byte minimum. Packet-capture
evidence remains deferred. The follow-up source adds a fixed four-host list,
confirmed per-host revoke, visible forget-all hint, and a two-phase first-pair
handoff: the Rust host persists its device pin before sending a commit
heartbeat, and the X4 persists the host pin only after receiving it. An
interruption therefore re-prompts instead of leaving an untracked one-sided
trust decision. That follow-up passes software gates and the firmware build but
still needs its compact physical UI/interruption check.

## Working features

- The Terminal activity uses CrossPoint's saved-network selector, advertises
  `_knietty._tcp.local`, and answers bounded UDP discovery. The new candidate
  wraps the accepted TCP socket in mutually authenticated TLS 1.3 with
  TCP_NODELAY before accepting a protocol greeting.
- Protocol v2 historically negotiated 80 x 24 and v1 supported the 50 x 22
  proof of concept. The Rust host retains both only behind explicit
  `--insecure-plaintext` compatibility; the current TLS firmware rejects them.
- Protocol v3 preserves v1 discovery metadata, then carries typed
  frames with an eight-byte network-order header and a 512-byte payload cap.
  Firmware uses one fixed 512-byte decoder payload and one fixed 1 KiB TX ring;
  these are allocated once with the Terminal activity and never per frame.
- The Milestone 08 transport candidate reads each framed TLS record in bounded
  chunks instead of invoking wolfSSL once per byte. Terminal consumes at most
  2 KiB or 2 ms per loop so a large PTY burst cannot starve buttons, network
  state, or render scheduling. The Rust bridge's default output ceiling is
  256 KiB/s; `--max-bps 65536` preserves the former ceiling for the hardware A/B.
- Matching peers advertise `burst1`. The host drains consecutive 512-byte PTY
  frames at their actual pacing deadline and emits a zero-payload boundary
  after 24 ms of PTY quiet. Firmware snapshots that complete burst, with an
  80 ms hard timeout after the latest payload and legacy batching until the
  first marker is observed.
- The TLS candidate uses TLS 1.3 only, self-generated P-256 identities, mutual
  certificate proof-of-possession, persistent pins, and a six-digit SAS derived
  from ordered device/host certificate hashes. The X4 pins up to four hosts and
  rejects a changed key under an existing host name. Discovery carries no
  terminal bytes and explicitly reports TLS as required.
- A deliberate X4 exit sends an empty v3 `SESSION_END` before Wi-Fi teardown.
  The foreground host exits cleanly on that frame; opt-in reconnect mode returns
  to discovery. Linux/macOS TCP keepalive bounds stale abrupt-loss sessions.
- An active v3 foreground bridge exposes a mode-`0600` per-device Unix socket
  in a mode-`0700` user runtime directory. A second `knietty` CLI can report
  status, request a safe HALF clean, or set explicit polarity through the same
  approved TCP stream. One command is bounded in flight. Mutations are quiet by
  default; ordinary clean runs after 500 ms of PTY silence, while `--wait
  --json` retains ordered PRESENTED/READY telemetry for measurement.
- `knietty diagnose --suite {smoke,latency,cadence,burst} --output PATH`
  negotiates the separate `frame,diag1` capability without spawning a PTY. Its
  fixed command set covers deterministic cell/row/scroll/window-boundary/large
  and 1/2/5/10/25/100-cell burst patterns, polarity, clean, and stop; raw
  controller controls are not exposed.
- Cadence sends six tagged updates at each of 600/400/200/100/50/25 ms. While
  one refresh executes, firmware merges later named requests into one fixed
  pending screen state and reports its exact first/last sequence and count.
  There is no per-command firmware allocation or dynamically sized queue.
- SSD1677 timing now distinguishes activation-to-BUSY completion, exact BUSY-fall
  presentation timestamp, post-waveform baseline, optional power-off, and final
  READY timestamp. Firmware sends fixed 108-byte network-order refresh records;
  JSON serialization and host monotonic timestamps remain host-only.
- The fixed terminal model supports delayed VT wrapping, scrolling, dirty
  column spans, cursor state, basic CSI/SGR, and incremental UTF-8. Invalid or
  non-BMP input consumes one replacement cell.
- The pending lock-in image replaces the inverse block cursor with a static
  one-pixel underline at the cell's unused bottom edge. It changes eight pixels
  rather than nearly the full 10 x 18 cell and keeps the glyph under the cursor
  readable. This is software-tested but not yet physically judged for residue.
- Terminus 8 x 16 is the default flash-resident terminal font. Spleen remains
  an explicitly selectable 1,001-glyph profile covering Latin, Greek, Cyrillic,
  box/block drawing, Braille, and a small Powerline subset. An eight-pixel left
  bezel inset is recovered from eight cell gutters, so all 80 columns still end
  exactly at pixel 800.
- Terminus (937 glyphs), Spleen, and GNU Unifont (978 glyphs) have compiled
  profiles. `docs/terminal-font-gallery.html` renders the exact firmware bitmap
  bytes for all three choices; this is not a browser-font approximation.
- The 32-pixel header shows `knietty@host`, an exactly centered clock, and an
  aligned battery percentage/icon. The first waiting frame seeds a real battery
  reading. Approval hints use the configured logical Confirm and Back mapping
  and the terminal plane is cleared when approval ends.
- Waiting mode shows the hostname, address, host command, and control tips.
  Left/Right toggles connected-terminal queue/render/LUT/plane/BUSY/baseline
  timing, region size, range/average, and window/fallback/settle/clean counts.
- Confirm sends Enter, long Confirm sends Ctrl+C, Back sends Escape, long Back
  toggles whole-screen polarity, and arrows send VT100 cursor sequences. Power
  requires a second press within three seconds to leave Terminal.
- Rendering snapshots the model under a short lock, redraws only dirty spans,
  and uses the X4 differential window path for regions requiring at most 8 KiB
  of temporary transfer memory. Large/unsupported updates fall back to the
  resident full framebuffer. Adaptive refresh keeps a one-phase custom SSD1677
  LUT resident during output bursts, bulk-uploads its 105-byte table, then uses
  a bounded forced-target DU settle. The default panel profile is restored
  before returning to CrossPoint.
- Terminal's renderer ownership is temporary: it captures the effective fading
  fix and fast-refresh profile, disables the extra fading-fix display pass while
  active, and restores the exact captured values on exit. Render requests that
  arrive during an E Ink update are coalesced into one guaranteed replay rather
  than being dropped.
- The host creates an isolated PTY session/process group. Local Ctrl+C is
  written to that PTY; Ctrl+\\ exits the bridge even during retry waits. An
  established disconnect exits by default, while `--reconnect` enables daemon
  rediscovery. systemd and launchd templates select the latter.
- tmux remains the preferred child command when installed, preserving the
  session across bridge/device reconnects; `$SHELL` is the fallback.
- The Rust host owns the protocol/discovery, PTY, foreground network bridge,
  and diagnostics implementation under
  `host-rs/`. It decodes fragmented/coalesced frames, rejects
  invalid flags and lengths, parses all current diagnostics metadata/events,
  handles 32-bit timestamp wrap, retransmits discovery every 250 ms, and emits
  stable device rows. It creates isolated process-group PTYs,
  configures geometry/environment, safely restores local termios/file flags,
  and reaps child processes through bounded HUP/TERM/KILL escalation. Its
  synchronous poll loop negotiates v3/v2/v1, keeps queues bounded, paces PTY
  output, relays local Ctrl+C, consumes Ctrl+\\ as the local exit, exits after
  an established disconnect by default, and preserves the PTY across opt-in
  reconnects. The foreground path is fake-device tested and has completed one
  physically confirmed X4/macOS terminal session; local Ctrl+C also behaved as
  expected. Rust diagnostics now run smoke, latency, cadence, and burst without
  a PTY, validate response/event sequence invariants, and write the frozen
  deterministic JSONL schema. The non-daemon macOS/Linux physical matrix is
  user-confirmed complete; exact Linux environment metadata remains unrecorded.
- Protocol v3 terminal output is capped at 512 payload bytes per frame. The
  bridge must therefore wake at its pacing deadline even when the previous
  encoded frame has drained; sleeping for the generic 100 ms poll interval
  reduces a nominal 256 KiB/s ceiling to roughly 5 KiB/s in practice.
- New matching peers negotiate optional `burst1`. The host emits a zero-payload
  boundary after 24 ms without new PTY output, and firmware presents the newest
  accumulated terminal state at that boundary. Firmware uses an 80 ms hard
  timeout and does not enable boundary gating until it observes the first
  marker, preserving interoperability with older hosts.

## Known failures

- The SD updater percentage has a software fix in `knietty-0.1.1`, but the
  updater performing that installation was still 0.1.0, so its 10% synchronous
  progress paints remain physically unobserved until the next SD update. The
  saved-Wi-Fi auto-connect fix is physically validated: Terminal rejoined the
  saved network without manual confirmation.
- The one-frame adaptive waveform has poor contrast and excessive ghosting.
  Adaptive 20 MHz is not fast enough to justify that quality loss. Adaptive
  40 MHz is meaningfully faster and usable as an experiment, but safe 20 MHz
  remains the best overall experience. In the baseline-v1 debrief the user
  reported occasional updates with no obvious visible reaction, some ghosting,
  and contrast better than the previous adaptive attempt but still inadequate.
- The nominal 100 ms / 20 MHz profile materially improves ordinary typing on
  the tested X4, but long text and btop sessions accumulate grainy gray residue.
  btop's clock also skips one or two displayed seconds at times despite btop
  already running at an explicit 500 ms interval. Final-state
  pruning was physically tested and did not resolve either symptom. A single
  rectangular refresh can still enclose unchanged pixels between separated real
  changes, but redundant TUI repainting is no longer the leading explanation.
  Suppressing the automatic 250 ms settle did not cure this in the first A/B,
  but that run was confounded by a newly identified battery-header repaint
  storm. Sustain1 substantially improved typing but did not prevent severe btop
  grain. Inversion temporarily clears the residue in both profiles.
- The plain X4 battery percentage was visibly oscillating between approximately
  68% and 78% during the no-settle test. Terminal called the ADC-backed battery
  path on every loop and scheduled a header repaint for every percentage change.
  The next candidate limits Terminal battery status sampling to once per minute;
  this fix is not yet physically validated.
- The new font is deliberately bounded and is not a full Nerd Font. Applications
  that require sixel, emoji, combining-cell shaping, or unimplemented xterm CSI
  behavior will still degrade.
- Linux non-daemon behavior was user-confirmed, but the exact distribution,
  version, Rust toolchain, and raw result files were not copied into this
  worktree. Repeat with those fields recorded before a release claim.
- The latest physically tested Wi-Fi image is the TLS candidate; the last
  committed hardware-known-good checkpoint is still the former plaintext
  build until the TLS source checkpoint is recorded. Its NVS private material
  is not protected against physical flash extraction because the X4 has no
  secure element. The follow-up per-host list/revoke and two-phase first-pair
  handoff are software-tested but not yet physically exercised.
- The Milestone 04 capture did not record ambient temperature, external-power
  state, optical onset, or a capture-specific subjective quality score. Its
  electrical timings are valid, but later optical/quality comparisons must
  record those missing conditions rather than infer them.
- The latest safe-profile diagnostics show the CrossPoint sunlight-fading fix
  was active during the first Terminal capture: it disabled every window update
  and powered the SSD1677 down after every refresh. The subsequent setting-off
  A/B test confirmed the approximately 200 ms penalty disappears and windowing
  resumes. Automatic temporary disable/restore and the approval-prompt render
  fix were subsequently validated on the physical X4.
- Directed broadcast can be filtered by guest Wi-Fi/client isolation; explicit
  `--host` is the fallback.
- The safe controller profile cannot approach 30 or 60 Hz: its measured BUSY
  waveform is about 503 ms even for one cell. Adaptive-40 completed almost all
  small-window 25 ms cadence requests electrically, but its known ghosting and
  weak contrast prevent a legible-frame-rate claim. Raw-panel high-refresh
  projects still have per-pixel waveform and drive controls the SSD1677 command
  interface does not expose.
- The default native CMake invocation on this development Mac does not find the
  Command Line Tools SDK libc++ headers. The explicit SDK include flag in Build
  commands is the tested workaround.

## Architecture findings

- Upstream baseline is `develop` at `33f07db7`; source version is CrossPoint
  1.5.0. Toolchain: pioarduino PlatformIO Core 6.1.19,
  platform-espressif32 55.3.37, Arduino-ESP32 3.3.7, ESP-IDF 5.5.2, and RISC-V
  GCC 14.2.0.
- The plain X4 board profile is 800 x 480 with an SSD1677 and one 48,000-byte
  1-bpp framebuffer. This is a source finding; the controller identity of the
  available physical unit has not been independently read back.
- `HalDisplay::FAST_REFRESH` maps to the SSD1677 fast waveform. Source comments
  document roughly 500 ms for the stock path, roughly 77 ms for the opt-in X4
  fast-DU shortcut, HALF at 1720 ms, and FULL around 1800 ms. These are source
  values, not measurements from knietty.
- FreeInk already implements byte-aligned SSD1677 rectangular differential
  refresh. knietty exposes it through `HalDisplay` and `GfxRenderer`, transforms
  logical orientation to panel memory, and caps its transient vector at 8 KiB.
  X3, factory-LUT, fading-fix, and larger regions use full-buffer fallback.
- The SSD1677 data sheet maps RED/BW RAM pairs 00/01/10/11 to LUT0/1/2/3.
  Terminal Turbo therefore idles unchanged black/white, drives black-to-white
  with one VSL phase, and white-to-black with one VSH1 phase. The profile is an
  explicit driver capability: unsupported controllers report PanelDefault, and
  exiting Terminal restores PanelDefault. FULL/HALF paths remain unchanged.
- SSD1677 RAM windowing changes which bytes are written, not the configured X4
  gate count. Driver Output Control remains at 480 lines and each
  `MASTER_ACTIVATION` is global. The W100 LUT leaves unchanged-black and
  unchanged-white entries idle, so it has no balanced sustain/restore phase to
  cancel small common-electrode/gate disturbances on untouched pixels. The
  physical whole-screen gray drift makes this the leading waveform hypothesis.
- Two independent W100 experiments implemented the next attribution step. The
  no-settle profile removes only the automatic 250 ms DU maintenance activation;
  it does not change the LUT. The sustain profile keeps scheduler behavior and
  divides the original twenty 5 ms frames into 5 ms balance, 5 ms restore, and
  90 ms target drive. The unchanged-black and unchanged-white states receive
  opposite two-phase pulses. Physical testing found near-perfect typing from
  Sustain1 but severe grain under btop, so equal pulse time is not sufficient
  evidence of electrical charge balance: VSH1, VSL, and VCOM are not symmetric
  drive levels. No-settle did not cure cadence in a run contaminated by rapid
  header updates. The combined retest removes that foreground repaint source.
- Terminal inversion marks the entire screen changed, and returning to normal
  drives it through the opposite full-frame transition before reseeding both
  controller RAM planes. Its repeatable temporary grain cleanup proves the
  residue is reversible, but does not by itself distinguish optical particle
  conditioning from a full-plane baseline-resynchronization effect. A later
  experiment must separate RAM-only reseeding, target-only drive, and the
  observed bidirectional scrub.
- SSD1677 Display Mode 2 exposes volatile RAM ping-pong through display-option
  register `0x37` bit F6. If it behaves as documented on the X4, the controller
  can swap old/new RAM roles after activation and eliminate knietty's manual
  post-refresh BW/RED baseline rewrite. Do not issue the separate waveform or
  display-option OTP programming commands; they are irreversible and provide no
  benefit over volatile register/LUT testing.
- Driver Output Control `0x01` changes the gate MUX count, but the documented
  SSD1677 range is 300-680 lines and scanning is edge-anchored rather than an
  arbitrary dirty-row window. A separate 800 x 300 speed viewport could test
  whether scanning 300 instead of 480 gates shortens BUSY, at the cost of about
  nine terminal rows. Ordinary RAM windowing reduces transfer bytes but leaves
  all 480 gates configured.
- Blocking display calls now record total, BUSY/waveform, and non-waveform time.
  The SSD1677 additionally records LUT upload, initial plane transfer, and
  post-waveform baseline synchronization. Terminal adds queue and render time,
  freezes the connected-session snapshot while showing diagnostics, and excludes
  non-terminal frames from its averages.
- `ActivityManager` renders on a separate FreeRTOS task. Terminal's main task
  already parses network output while that task blocks in the SSD1677 BUSY
  wait, snapshots the newest model only while holding a short mutex, and
  coalesces new render requests into one follow-up. Consequently a start/finish
  split by itself cannot explain or remove multi-second btop lag. True CPU
  rendering overlap would also need a bounded active-window staging buffer;
  full-frame FAST fallback cannot permit framebuffer mutation during BUSY
  without a second 48 KiB copy or a different proven controller baseline.
- Before this milestone, `TerminalWifi::pollFramedClient()` called
  `wolfSSL_read(..., 1)` for every v3 header and payload byte, and
  `TerminalActivity::pollWifi()` consumed only 256 payload bytes per pass. This
  is a concrete full-screen TUI throughput candidate independent of panel BUSY.
  The new decoder reports exactly how many bytes remain in its current bounded
  header/payload so TLS can bulk-read without consuming into the next frame.
- `GfxRenderer::displayWindow()` deliberately falls back whenever the global
  sunlight-fading fix is enabled, and full-buffer rendering passes that setting
  to the driver as `turnOffScreen=true`. The X4 `0xFC` partial sequence otherwise
  keeps the controller powered, so the driver follows it with its separate
  power-off operation containing a fixed 200 ms delay. The safe hardware capture
  below has 201.3 ms of transfer time not attributed to planes or baseline,
  matching that path to measurement precision.
- The SSD1677 retains a custom LUT in controller RAM until an OTP/default
  activation, reset, sleep, or profile transition invalidates it. Adaptive burst
  updates therefore avoid re-uploading 105 LUT bytes one SPI transaction at a
  time; the first upload is now one bulk transaction.
- Modos/Caster-style 60 Hz work drives raw panel source/gate buses with FPGA and
  per-pixel state. On the X4, MASTER_ACTIVATION starts one global SSD1677
  waveform and BUSY prevents the next activation, so those algorithms cannot be
  transplanted through software alone. The realistic optimization space is
  bounded transfer, shorter/better waveforms, coalescing, and perceived-latency
  staging.
- Bundled CrossPoint UI fonts are proportional. The generated Spleen table is
  18 bytes per glyph in flash; switching to Unicode cells raises each terminal
  model from about 3.8 KiB to 7.7 KiB. Terminal owns two bounded models so RX can
  continue during display work. Linker-reported static RAM did not increase.
- X3/X4 exposes logical Back, Confirm, Left, Right, Up, Down, and Power.
  Terminal uses `MappedInputManager`, not physical GPIO IDs.
- CrossPoint network activities stop mDNS, disconnect Wi-Fi, and use
  `silentRestart()` when leaving. Terminal follows that lifecycle.
- The knietty build omits serial logging. Native USB is Arduino-ESP32 `HWCDC`,
  but no CDC node appeared on the tested locked unit, so Wi-Fi is primary.
- `KNIETTY_STABLE_POWER` is isolated to the feature environment: it disables
  the branch's experimental BUSY-slice/main-loop light-sleep behavior, closes
  the CDC object before deep sleep, and retains 1.5.0 quick-resume semantics.
- Protocol v1/v2 becomes an unframed byte stream after approval. Arbitrary
  terminal bytes therefore cannot safely share that stream with telemetry by
  reserving an escape sequence. Diagnostics should introduce a negotiated v3
  framed stream on the existing discovery service and TCP port; v1/v2 must
  remain available for compatibility.

### Host-controlled diagnostics design

- `KNIETTY/3` negotiates `terminal` or `diagnostics` mode and capabilities in
  the greeting. Diagnostics uses the existing named-host approval screen but
  explicitly says that the host is requesting a display test. It does not
  spawn a PTY or mix shell output into the measurements.
- After approval, both directions use a bounded binary frame: type, flags,
  16-bit payload length, and 32-bit sequence number. Frame types cover terminal
  data, device input, control request/response, refresh telemetry, presented
  acknowledgement, and heartbeat. TCP already provides ordering and integrity;
  TLS can later wrap this unchanged stream. Reject unknown types and payloads
  above a small fixed limit rather than allocating from an untrusted length.
- The frozen diagnostics JSON Lines contract contains one immutable
  session/build record followed by accepted, PRESENTED, and READY records for
  every refresh request. The Rust implementation preserves that schema and the
  self-contained v3 golden fixture under `host-rs/fixtures`.
- Rust protocol and discovery remain standard-library based. The PTY layer uses
  `nix` 0.31.3, whose supported Rust floor is below the installed Rust 1.80.1,
  for typed Darwin/Linux PTY, termios, descriptor, process, and signal wrappers.
  The unavoidable post-fork `setsid`/`TIOCSCTTY` operations and window ioctls
  are isolated in `pty.rs` with explicit safety invariants; no unsafe code is in
  the protocol, discovery, CLI, or terminal-guard modules.
- Rust now provides the foreground `connect` CLI; persistent daemon supervision
  is explicitly deferred to the Linux integration backlog. Discovery converges in either startup
  order through periodic host
  probes plus the X4's mDNS/rate-limited availability announcement; TCP remains
  host-initiated and physical approval remains device-owned. Each host and X4
  has a persistent ID. Unpaired devices are listed rather than auto-claimed,
  automatic connection requires an explicit device-to-host assignment, one X4
  accepts one pending/active session, and one daemon may supervise multiple
  assigned X4s with separate tmux sessions. IDs remain routing hints until TLS
  binds them cryptographically in Milestone 06.
- Firmware timestamps remain monotonic and relative, so host and X4 clocks do
  not need synchronization. The `PRESENTED` timestamp is captured exactly when
  BUSY falls and `READY` follows baseline synchronization/power handling; this
  distinction directly measures work that delays the *next* activation after
  the new image is already on the panel. The current blocking renderer sends
  both events after READY, so device timestamps—not host receive spacing—measure
  that gap. Each event identifies the first and last included sequence and
  coalesced count. Final telemetry reports
  RX/parse/queue/render time, LUT upload, first-plane transfer,
  activation-to-BUSY assertion, BUSY/waveform, baseline synchronization,
  power-off, total display time, actual dirty/aligned rectangle and bytes,
  changed rows/cells, requested and actual refresh path, and a stable
  fallback-reason code. The host separately records send and event-receive
  monotonic times for end-to-end latency.
- Each session record includes firmware and FreeInk revisions, diagnostics
  schema, board/controller/resolution, safe/adaptive profile, SPI frequency,
  font/orientation/polarity, sunlight-fading state, battery, free/minimum heap,
  Wi-Fi RSSI, and host OS/version. Ambient and panel temperature must be entered
  as external observations unless a trustworthy panel sensor is identified.
- Initial bounded suites are: `smoke`; `latency` for top/middle/bottom cells,
  adjacent cells, cursor-sized, one-row, disjoint-row, scroll, 8 KiB boundary,
  and near-full regions in both directions; `cadence` at
  25/50/100/200/400/600 ms input spacing; and deterministic
  1/2/5/10/25/100-cell `burst` updates. An opt-in
  `ghosting` suite alternates known patterns for bounded counts and finishes
  with a clean. A phone high-speed-video capture of host action plus panel is
  still required to measure visible onset; SSD1677 BUSY completion is only a
  firmware-observable proxy for presentation.
- The host may select only compiled, whitelisted patterns and safe profile
  choices. Cap repetitions and duration, rate-limit commands, abort on Power,
  Back, disconnect, or timeout, and restore orientation, display profile,
  sunlight-fading state, and controller power state on every exit. Never expose
  raw SSD1677 register writes, voltage controls, OTP commands, or an arbitrary
  overclock command over the network.

## Hardware observations

- Device: China-locked XTEINK X4, initially running CrossPoint 1.4.1 installed
  through the web unlock tool. The exact 1.4.1 application binary is retained.
- The proof-of-concept knietty image booted and its Wi-Fi terminal was approved,
  discovered, connected, and used interactively. The user signed it off.
- LAN discovery found `knietty-9e54a0` at `192.168.0.251:29380`.
- The proof-of-concept's SD updater failed at displayed 1% for both a polished
  knietty image and the retained 1.4.1 image. The active image stayed bootable.
- CrossPoint's normal network OTA then restored official base 1.5.0 without the
  Unlocker. From that clean base, the SD-menu update to
  `knietty-0217ada8-80x24-sd-safe.bin` completed successfully and booted. This
  establishes normal OTA as recovery and the current SD application path as a
  viable custom update path.
- The installed `0217ada8` terminal confirmed 80 x 24 geometry, Wi-Fi operation,
  connection approval, and two-press exit. It also produced the UI, glyph,
  disconnect, latency, and sleep failures listed above.
- The subsequent `60c30d06` SD flash succeeded. On that image the Home icon is
  correct, normal sleep/wake works, exiting Terminal cleanly disconnects the
  default host bridge, btop and box drawing work, and physical connection
  approval still accepts/denies correctly. Remaining observations are the
  invisible approval legend, left-edge clipping, occasional black/white flashes,
  and roughly 500 ms perceived burst cadence.
- The subsequent Terminus turbo artifact also flashed successfully. Terminus is
  preferred, waiting tips and diagnostics render, btop works, and Turbo is
  visibly faster, but the tips need rotation to the right edge, the left inset
  needs another four pixels, and the waveform has excessive ghosting and weak
  contrast.
- The `500d757d` safe 20 MHz, adaptive 20 MHz, and adaptive 40 MHz artifacts
  were subsequently flashed successfully. Safe is the preferred-quality build.
  Adaptive 20 MHz is an unattractive middle ground; adaptive 40 MHz is
  noticeably faster and the only adaptive profile currently fast enough to
  justify experimentation, but it retains weak contrast and ghosting.
- The user validated the `4db85157` Gate A safe artifact. The approval prompt
  that previously accepted input without painting is visible, and the
  terminal-owned display-state/sleep-wake checklist passed. No quantitative
  timing capture was taken during this gate.
- The user validated the `fb517134` Gate B safe artifact. Ordinary v3 terminal
  operation, forced v2 compatibility, diagnostics approval/deny/abort, smoke
  JSONL output, cleanup, and post-exit Home sleep/wake passed. The user reported
  no failure in this checklist.
- The user SD-flashed `knietty-W100-e4238425-20MHz-EXPERIMENTAL.bin` and ran the
  bounded smoke suite successfully. Ordinary typing is much better. Full-screen
  btop remains less responsive, sometimes skipping one or two displayed clock
  seconds, and long typing/TUI repaint sessions accumulate grainy gray residue.
- The user SD-flashed
  `knietty-CODEX-PARSER-c41de6e3-base-W100-SUSTAIN1-NOSETTLE-BATT60-UNDERLINE-20MHz-EXPERIMENTAL.bin`
  and exercised Codex on the X4 from macOS. Scrolling was reported good and the
  phantom `q` disappeared. The remaining `?` is Codex's warning icon and is not
  treated as terminal corruption. This parser-only artifact does not contain
  the later explicit-session-close fix.
- The user then SD-flashed the matching
  `knietty-CODEX-PARSER-SESSIONEND-c41de6e3-base-W100-SUSTAIN1-NOSETTLE-BATT60-UNDERLINE-20MHz-EXPERIMENTAL.bin`
  and ran the updated Rust host on macOS. Exiting Terminal closed the host
  gracefully and returned to CrossPoint. Abrupt Wi-Fi/power-loss keepalive
  timing was not measured in this check.
- The user SD-flashed the TLS candidate based on `c07520d4` and tested it from
  macOS. First pairing, trusted reconnect, terminal traffic, runtime display
  commands, graceful exit, ordinary sleep/wake, diagnostics approval, and the
  two-step X4 forget-all gesture worked without a reported bug or observable
  TLS lag. `knietty display status` reported the `terminal-interactive` profile,
  80 x 24 Terminus geometry, RSSI -46 dBm, 63,828 bytes free heap, and 45,800
  bytes minimum free heap. The status JSON is retained at
  `results/2026-08-16-x4-tls-status.json`. No packet capture was available, and
  the waiting screen had no discoverable forget-all hint.
- During the Milestone 04 campaign, mDNS proved that an initially selected
  image was an older adaptive-20 build (`proto=2` and the on-device label
  `Adaptive DU / 20 MHz experimental`), not the intended safe baseline. The
  short artifact alias `knietty-M4-ae82c301-SAFE.bin` was then flashed and the
  complete safe campaign passed. The first adaptive-40 shell loop exposed a
  one-shot host discovery race while the X4 cleaned up the preceding session.
  After the host began re-probing every 250 ms, all four adaptive-40 suites also
  passed.
- No USB CDC `/dev/cu.usbmodem*` node was observed on the connected Mac.
- No partition table, bootloader, secure-boot, or eFuse changes were made.

- The user SD-flashed the clean, current-upstream `knietty-0.1.2` candidate and
  reported the complete requested gate as clean: saved Wi-Fi reconnected
  automatically; approval, TLS, typing, and btop worked; display commands and
  graceful exit worked; reader operation resumed after Terminal; and ordinary
  sleep/wake outside Terminal worked. This is an X4/macOS observation and is
  not evidence of Linux parity.

## Linux host observations

- On 2026-08-16 the user confirmed the complete non-daemon Rust matrix passed
  on a Linux host: software gate, discovery, explicit IP, approval/denial,
  shell/tmux, Ctrl+C/Ctrl+\\, disconnect/reconnect, four diagnostics suites,
  interrupt cleanup, and X4 sleep/wake afterward.
- The exact Linux distribution/version, Rust/Cargo versions, source revision,
  firmware label, and raw JSONL files were not copied into this checkout. This
  is valid user-reported validation, but insufficient metadata for a release
  evidence bundle.
- The systemd user template is migrated to the Rust binary. Persistent daemon
  supervision and an installed service test remain backlogged.

## macOS host observations

- Development host: Darwin/macOS, shell `/bin/zsh`.
- The final legacy host oracle passed 39/39 tests before deletion. Coverage
  included discovery and protocol
  parsing, portable PTY sizing/environment, raw local terminal restoration,
  Ctrl+\\ during retry, log rate limiting, and a live subprocess assertion that
  Ctrl+C signals only the PTY child process group. It now also covers diagnostic
  codecs, ordered JSONL telemetry without a PTY, and 32-bit device-clock wrap.
- The proof-of-concept, `0217ada8`, and `60c30d06` bridges were physically
  exercised through discovery, approval, connection, and interactive output.
  `60c30d06` confirmed the default bridge exits on a clean device disconnect.
- `fb517134` was physically exercised on Darwin 25.5.0 through ordinary v3,
  forced v2, and the separately approved diagnostic path. Its smoke capture
  completed with no rejected commands and wrote a structurally complete JSONL
  result.
- Automatic Wi-Fi discovery now retransmits every 250 ms throughout its bounded
  timeout. This fixes the observed back-to-back campaign race where the next
  process sent its sole UDP probe before the X4 returned to listening. The host
  suite passes 38/38 with a deterministic missed-first-probe regression test.
- LaunchAgent behavior has not been tested as an installed user agent.
- Rust 1.80.1/Cargo 1.80.1 builds the host crate with `nix` 0.31.3. The current
  matrix passes 49 Rust unit tests, two process-cleanup integration tests,
  strict Clippy, the optimized build, and PTY smoke, including a real loopback UDP
  missed-first-probe test, PTY geometry/environment, isolated process groups,
  Ctrl+C delivery to only the child group, child reaping, and terminal restore
  on normal drop, panic unwind, and SIGTERM during an approval wait. Loopback
  TCP peers also cover v3 framed traffic, protocol fallback, denial, malformed
  approval, established disconnect, and reconnect with the same PTY child. The
  user physically confirmed the complete non-daemon foreground/diagnostics
  matrix with the X4 on macOS, including local Ctrl+C/Ctrl+\\, reconnect,
  denial, cleanup, and post-diagnostics sleep/wake. A 41-line smoke artifact is
  retained at `results/rust-host-matrix/Darwin-smoke.jsonl`.
- The TLS host uses rustls 0.23.23 with the ring provider and configures only
  TLS 1.3 with 2 KiB record fragments. rcgen 0.13.2 creates the persistent host
  P-256 identity. A bounded custom verifier accepts one self-signed leaf for
  first-pair SAS verification and requires an exact SHA-256 certificate match
  after pinning; rustls still verifies the handshake signature. The firmware
  uses the already pinned Arduino-wolfSSL 5.7.2 library, enables its server half
  and compact retained-peer-certificate support, and uses mbedTLS only for the
  first-boot P-256 key/certificate generation already available in ESP-IDF.

## Build commands

```sh
cd /Users/rodrigomtorres/git/knietty/crosspoint-reader

env PATH="$PWD/.venv/bin:$PATH" ./bin/clang-format-fix -g

./host-rs/scripts/check.sh

native_test_dir=$(mktemp -d /tmp/knietty-tests.XXXXXX)
cmake -S test -B "$native_test_dir" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_CXX_FLAGS='-isystem /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk/usr/include/c++/v1'
cmake --build "$native_test_dir" -j4
ctest --test-dir "$native_test_dir" --output-on-failure

$HOME/.platformio/penv/bin/pio run -e knietty_safe
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_oc
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_100ms
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_100ms_nosettle
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_100ms_sustain
$HOME/.platformio/penv/bin/pio run -e knietty_adaptive_100ms_sustain_nosettle
$HOME/.platformio/penv/bin/pio run -e knietty_mode2_pingpong
$HOME/.platformio/penv/bin/pio run -e knietty_async_window

python3 scripts/generate_terminal_font_gallery.py
```

Formatting passes with clang-format 21.1.8 installed in the local firmware
development environment. The final legacy oracle passed 39/39 before removal;
the Rust host passes 49 unit and two process-cleanup integration tests plus
strict Clippy, an optimized release build, and a live PTY smoke; the native
suite passes 167/167. The
Milestone 04 checkpoint `ae82c301` safe build reports RAM 54,268 / 327,680
bytes and flash 5,649,385 / 6,553,600 bytes. Adaptive 40 MHz reports RAM 54,292
bytes and flash 5,650,353 bytes. The expanded suites add no linker RAM to safe
and no dynamic firmware queue; their fixed aggregation metadata lives in the
Terminal activity's one-time heap allocation. These are linker figures, not
runtime heap measurements.

The Milestone 06 TLS checkpoint passes the relevant experience
firmware build, 49/49 Rust unit tests, 2/2 Rust process-cleanup integration
tests, strict Clippy, the optimized Rust build, PTY smoke, and 167/167 native
tests on Darwin. The repository clang-format 21 wrapper passes. The linker
reports 54,292 / 327,680 bytes RAM and 5,701,317 / 6,553,600 bytes flash (16.6%
and 87.0%). The first-boot certificate writer uses the fixed identity member
buffers rather than placing its 768-byte certificate and 256-byte key
workspaces on the C3 task stack. The real TLS handshake heap floor remains a
physical measurement. `pio check --fail-on-defect medium` still fails on the
pre-existing cppcheck `TerminalScreen::cells` constructor false positive; it
also reports the pre-existing low-level `end` name shadow in v3 parsing. The
new TLS pair-store always-true return finding was removed.

The first Milestone 07 Mode-2 candidate passed the 49-test Rust unit suite, two Rust
process-cleanup integration tests, strict Clippy, optimized build and PTY smoke,
173/173 native tests, formatting, its dedicated firmware build, and a no-flag
W100 Sustain1/no-settle regression build. The experimental linker figures are
54,308 / 327,680 bytes RAM and 5,702,141 / 6,553,600 bytes flash. The no-flag
regression reports 54,292 bytes RAM and 5,701,477 bytes flash. Its physical
smoke capture was internally valid, but the observed stale-bank alternation
rejected the zero-copy design.

The synchronized-bank follow-up passes 174/174 native tests, its dedicated
firmware build, and the ordinary no-flag W100 Sustain1/no-settle regression.
The linker reports 54,308 / 327,680 bytes RAM and 5,702,321 / 6,553,600 bytes
flash for the experiment; the no-flag regression remains at 54,292 bytes RAM
and 5,701,477 bytes flash.

The earlier Milestone 08 transport candidate passes 49 Rust unit tests,
two process-cleanup integration tests, strict Clippy, the optimized Rust build,
and its firmware/control builds. The user physically A/B tested its former
64 KiB/s and new 256 KiB/s host pacing limits and saw no meaningful difference.
That test changed the configured ceiling but was later found to be masked by a
100 ms event-loop wake defect, so it neither establishes a performance
improvement nor excludes host delivery cadence.

The metrics-enabled async-window follow-up passes the complete 176/176 native
suite, 51 Rust unit tests, two Rust process-cleanup integration tests, strict
Clippy, the optimized host build/PTY smoke, formatting/diff checks, its dedicated firmware build,
and the ordinary no-flag `knietty_adaptive_100ms_sustain_nosettle` regression
build. PlatformIO reports 54,692 / 327,680 bytes linker RAM and 5,705,443 /
6,553,600 bytes flash for the experiment, versus 54,692 bytes linker RAM and
5,702,967 bytes flash for the control. The experiment's 8 KiB staging buffer is
lazily allocated at runtime and therefore is not included in linker RAM. The
user subsequently confirmed no regressions and no visible cadence gain.
The several-window SSD1677 contract is coherent on this X4, but the experiment
does not solve fullscreen TUI lag.

The `knietty-0.1.0` burst-delivery candidate passes 52 Rust unit tests, two
process-cleanup integration tests, strict Clippy, the optimized host build and
PTY smoke, 176/176 native tests, formatting, and the dedicated
`knietty_async_window` firmware build. Its fake-server regression delivers a
4,096-byte PTY write as eight consecutive 512-byte frames and a final boundary
in under 400 ms; the former 100 ms sleep would take roughly 700 ms before even
sending the boundary. The firmware image is 5,720,144 bytes and embeds build
identity `knietty-0.1.0`. The user physically confirmed fast, coherent btop
loading and updates; the exact live metrics are recorded below.

At the `0.1.0` gate, only that candidate was packaged; no additional safe or
experimental image was produced. Its SHA-256 is
`b564ef71db873de600c28a96825a0b500bd5805d5eaedeb2878b86eaa7f9503a`:

```text
/Users/rodrigomtorres/git/knietty/knietty-0.1.0.bin
```

The matching Rust host was installed at
`/Users/rodrigomtorres/.cargo/bin/knietty` and reports version `0.1.0`.

The firmware-only `knietty-0.1.1` follow-up keeps the host protocol and the
80 ms burst fail-safe unchanged. It fixes the render timestamp race, restores
serialized 10% SD-update progress, and gives saved credentials one bounded
post-scan retry. It passes formatting, 176/176 native tests, and the dedicated
`knietty_async_window` build. Its physical gate is pending; the packaged image
is 5,720,400 bytes with SHA-256
`1d5decedbafb04a2eca1a8929beb577fad19fa9b998b7dc742b9dca7f370744a`:

```text
/Users/rodrigomtorres/git/knietty/knietty-0.1.1.bin
```

Only the metrics-enabled async-window experiment was copied; no new safe or
Mode-2 image was generated. It is 5,719,296 bytes with SHA-256
`a2f8ce878292c64928864ee3c6572dbed44e9e0602ff7d3e15044595d656b87e`:

```text
/Users/rodrigomtorres/git/knietty/knietty-M8-ASYNC-WINDOW-METRICS-e645ff96-base-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
```

The same bytes are also copied under the short SD-picker-safe name
`/Users/rodrigomtorres/git/knietty/knietty-M8-METRICS.bin`. This avoids confusing
the metrics image with the preceding async image when a small display truncates
their shared long filename prefix.

Only the async-window experiment was copied; no new safe or Mode-2 image was
generated. It is 5,718,736 bytes with SHA-256
`74f9cea0034d40d2037d35f58850b38d231da892e6763fdf3dd58298764ffa3b`:

```text
/Users/rodrigomtorres/git/knietty/knietty-M8-ASYNC-WINDOW-e645ff96-base-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
```

The `-base` marker means the binary embeds parent `e645ff96`; all Milestone 08
changes remain uncommitted until the X4 hardware gate passes.

Only the current experience candidate was copied; no safe or Mode-2 image was
generated. It is 5,715,520 bytes with SHA-256
`07e545a64b681f4bd92056c04abc4d1e3b412d35da7835bdd830f721630ddf01`:

```text
/Users/rodrigomtorres/git/knietty/knietty-M8-RX-BULK-e645ff96-base-W100-SUSTAIN1-NOSETTLE-20MHz-CANDIDATE.bin
```

The RX-bulk image also embeds parent `e645ff96`; its Phase A result was
physically negative as described above.

The synchronized candidate is 5,716,176 bytes with SHA-256
`98370a58aad1b034b68077a93142e1b8bd3cca5dfbb93eac62860a54c4f7bbf2`:

```text
/Users/rodrigomtorres/git/knietty/knietty-M7-MODE2-SYNC-61e61088-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
```

Only the requested Mode-2 experiment was copied; no new safe artifact was
produced:

```text
/Users/rodrigomtorres/git/knietty/knietty-M7-MODE2-PINGPONG-4f105b1f-W100-SUSTAIN1-NOSETTLE-20MHz-EXPERIMENTAL.bin
```

It is 5,715,833 bytes with SHA-256
`7ab515b7ab154de00feffcf2fae91a88a905f90d6ebdaf8b526540888a9a6811`.

Only the requested current experience artifact was produced; no additional
safe image was built or copied:

```text
/Users/rodrigomtorres/git/knietty/knietty-TLS-CANDIDATE-c07520d4-base-W100-SUSTAIN1-NOSETTLE-BATT60-20MHz-EXPERIMENTAL.bin
```

It is 5,712,176 bytes with SHA-256
`f667fdb2cb180f9b224b5e65f967e758043e1d3c7e6268ebe2147479536c7912`.
The `-base` marker is deliberate because this physically tested artifact was
built before the TLS source checkpoint commit.

The nominal 100 ms / 20 MHz source checkpoint passes formatting, 38/38 host
tests, 160/160 native tests, its dedicated firmware build, and the unchanged
safe firmware build. Before final version stamping, the experimental build
reported RAM 54,292 / 327,680 bytes and flash 5,650,719 / 6,553,600 bytes; safe
reported RAM 54,268 bytes and flash 5,649,753 bytes. The extra LUT is 112 bytes
of flash-resident constant data and adds no runtime allocation. These figures
are software-only until the image is flashed and measured on the X4.

The final-state pruning candidate passes formatting, 38/38 host tests, 162/162
native tests, its nominal 100 ms build, and the unchanged safe-profile build.
The final experimental build reports RAM 54,292 / 327,680 bytes and flash
5,651,083 / 6,553,600 bytes; the safe regression reports RAM 54,268 bytes and
flash 5,650,097 bytes. The comparison reuses the existing render snapshot and
adds no framebuffer or repeated heap allocation.

The W100 scheduler/sustain candidates pass formatting, 38/38 host tests,
162/162 native tests, both dedicated firmware builds, and the safe-profile
regression build. No-settle reports RAM 54,292 / 327,680 bytes and flash
5,651,191 / 6,553,600 bytes. Sustain reports the same RAM and flash 5,651,275
bytes. Safe reports RAM 54,268 bytes and flash 5,650,287 bytes. The sustain LUT
adds 112 flash-resident bytes, no heap allocation, framebuffer, or stack buffer.
Both experimental images subsequently SD-flashed and ran on the physical X4;
their qualitative verdict is recorded above. The safe build figure is a
software regression result, not a new safe-image hardware test.

The two uncommitted, hardware-tested A/B artifacts based on source version
`c41de6e3` are:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-NOSETTLE-c41de6e3-base-20MHz-EXPERIMENTAL.bin
/Users/rodrigomtorres/git/knietty/knietty-W100-SUSTAIN1-c41de6e3-base-20MHz-EXPERIMENTAL.bin
```

No-settle is 5,665,040 bytes with SHA-256
`cb0a183679a5cebf57a8a4c63b7c95f66c384a140621757387eed8888fff4bc5`.
Sustain1 is 5,665,120 bytes with SHA-256
`1e6a89037b6219e76ecca15cc0cf748db1d49546f327ae0618b5d40f31a247a2`.
The `-base` marker is deliberate: version stamping names the last committed
parent, while these hardware candidates remain uncommitted until their physical
result is known. On-device timing labels and diagnostic session flags distinguish
them: no-settle reports bits `0x10|0x20`; Sustain1 reports `0x10|0x40`, in
addition to the shared adaptive bit `0x04`.

The post-A/B combined candidate is:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-SUSTAIN1-NOSETTLE-BATT60-c41de6e3-base-20MHz-EXPERIMENTAL.bin
```

It is 5,665,184 bytes with SHA-256
`3c571d5ef69308324c9861a9a37eda364511d264c6326b5f68b9bf0dde6d4db3`.
It reports diagnostic session flags `0x04|0x10|0x20|0x40` and the on-device
label `W100 sustain / no settle / 20 MHz experimental`. Formatting, 38/38 host
tests, 162/162 native tests, its dedicated build, and the safe regression pass.
The combined build reports RAM 54,292 / 327,680 bytes and flash 5,651,329 /
6,553,600 bytes. Safe reports RAM 54,268 bytes and flash 5,650,421 bytes. The
battery throttle adds no heap allocation and does not alter CrossPoint's reader
battery behavior; it only changes how often Terminal refreshes its header.

The underline-cursor lock-in image supersedes that otherwise identical artifact:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-SUSTAIN1-NOSETTLE-BATT60-UNDERLINE-c41de6e3-base-20MHz-EXPERIMENTAL.bin
```

It is 5,665,200 bytes with SHA-256
`8a03055f1a4095f7d60fe92ef94e3e90d9f7174ecd6b440f22fe3fe68439a04a`.
The dedicated build reports RAM 54,292 / 327,680 bytes and flash 5,651,349 /
6,553,600 bytes; the safe regression reports RAM 54,268 bytes and flash
5,650,441 bytes. It remains uncommitted until the cursor and combined profile
are confirmed on the physical X4.

The capture-derived Codex parser candidates are built from the same uncommitted
`c41de6e3` parent and FreeInk `2218b6c`:

```text
/Users/rodrigomtorres/git/knietty/knietty-CODEX-PARSER-c41de6e3-base-SAFE-20MHz.bin
/Users/rodrigomtorres/git/knietty/knietty-CODEX-PARSER-c41de6e3-base-W100-SUSTAIN1-NOSETTLE-BATT60-UNDERLINE-20MHz-EXPERIMENTAL.bin
```

Safe is 5,665,808 bytes with SHA-256
`de4671d8acb396879f690cd1e89623d6c6d8bedea1765197665446909746b0c8`.
The matching experience profile is 5,666,688 bytes with SHA-256
`c71137c3eb1089d81f17e20d6b231ceab9ef2ca57d1f7d1570814876b2e61706`.
Both compile successfully; formatting and 166/166 native tests pass. The user
physically validated the experience profile on the available X4 and reported
good Codex scrolling with the phantom `q` eliminated. The remaining visible
`?` represents an intentional warning icon. The matching safe image remains
software-tested only.

The combined parser and explicit-session-close candidates are:

```text
/Users/rodrigomtorres/git/knietty/knietty-CODEX-PARSER-SESSIONEND-c41de6e3-base-SAFE-20MHz.bin
/Users/rodrigomtorres/git/knietty/knietty-CODEX-PARSER-SESSIONEND-c41de6e3-base-W100-SUSTAIN1-NOSETTLE-BATT60-UNDERLINE-20MHz-EXPERIMENTAL.bin
```

Safe is 5,665,968 bytes with SHA-256
`d489d82cbe0a19d8086dbc340273ff9a235ca91351aae39edd12d70bc1dcd5d3`.
The matching experience profile is 5,666,864 bytes with SHA-256
`5f58e7a6a4f2a2d5880ce520c47fce4ba8f316f8cc13ad515bba71e832e173c6`.
They embed parent `c41de6e3` and FreeInk `2218b6c`. Both firmware builds,
formatting, 166 native tests, 43 Rust tests, strict Clippy, and the optimized
Rust build pass. The host loopback test proves `SESSION_END` terminates the
bridge while the simulated X4 deliberately keeps its TCP socket open. The user
then physically validated a graceful host exit with the combined experience
image and updated Rust host on the available X4/macOS setup. Abrupt-loss
keepalive timing remains unmeasured on hardware.

The in-session runtime-control hardware candidates are:

```text
/Users/rodrigomtorres/git/knietty/knietty-RUNTIME-CONTROL-61677477-SAFE-20MHz.bin
/Users/rodrigomtorres/git/knietty/knietty-RUNTIME-CONTROL-61677477-W100-SUSTAIN1-NOSETTLE-BATT60-20MHz-EXPERIMENTAL.bin
```

Safe is 5,666,528 bytes with SHA-256
`f8f34168c87e6d89c3f3fcd1e694987b9d01f5021d91d83ee9b7249def305068`.
The matching experience profile is 5,667,456 bytes with SHA-256
`ee82b8a0a5a150a36a942dd9a29bb38d758b32f8b93727a891c528024f92bd26`.
Both embed parent `61677477` and FreeInk `c0c059e`; formatting, 48 Rust
tests/strict Clippy/release/PTY smoke, 167 native tests, and both firmware
builds pass. They are software-tested only. Flash safe first through the normal
CrossPoint SD update path; do not use USB, esptool, or the Unlocker.

The retained nominal 100 ms / 20 MHz experiment is:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-e4238425-20MHz-EXPERIMENTAL.bin
```

It is 5,664,576 bytes and has SHA-256
`274d0ac1f4107f866baa76cc994326b0e2175553b095ce1aa2fcb64a95d27bf1`.
It embeds CrossPoint version
`1.5.0-dev-feature/knietty-terminal-e4238425` and FreeInk revision `2218b6c`.
This artifact passed its SD flash, ordinary typing/btop observation, and bounded
smoke test. It remains experimental because the observed grain and full-screen
cadence are not release quality. The retained `knietty-M4-ae82c301-SAFE.bin`
remains the hardware-tested rollback control.

The final-state pruning A/B candidate is:

```text
/Users/rodrigomtorres/git/knietty/knietty-W100-DIFF-ab5d5784-20MHz-EXPERIMENTAL.bin
```

It is 5,664,928 bytes and has SHA-256
`f43358bd90dec8a269eee050d967f9c7e9392abca61924ac25e46909ee7413f1`.
It embeds CrossPoint version
`1.5.0-dev-feature/knietty-terminal-ab5d5784`, FreeInk revision `2218b6c`,
and the `Adaptive 100 ms / 20 MHz experimental` profile. It is software-tested
only until the physical A/B check below is completed.

The Milestone 04 baseline artifacts are:

```text
/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-SAFE.bin
```

The safe 20 MHz image is 5,663,232 bytes and has SHA-256
`49aacc5b1c32da48e0a4e09bf9c76be40d3ce198559200b68c29b93ad3d451b3`.

```text
/Users/rodrigomtorres/git/knietty/knietty-M4-ae82c301-ADAPT40-EXPERIMENTAL.bin
```

The adaptive 40 MHz image is 5,664,208 bytes and has SHA-256
`aff854e1f46218e966ffc77a0f5df041d135fb830316ca3ec15ace5afa6c070f`.
It remains explicitly experimental because its SSD1677 SPI rate exceeds the
board's normal 20 MHz setting. Both images embed CrossPoint version
`1.5.0-dev-feature/knietty-terminal-ae82c301` and FreeInk revision `0ff05c6`.
They are software-validated only until the two matched physical captures pass.

The Milestone 01 Gate A artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-4db85157-80x24-terminus-safe-20mhz-gate-a.bin
```

It was rebuilt from source checkpoint `4db85157`, is 5,655,456 bytes, embeds
version `1.5.0-dev-feature/knietty-terminal-4db85157`, and has SHA-256
`bd45f2f807ea71e81ce64d30d85a73cc1ad77b2bfade65a2e30d466fe23efc24`.
The user subsequently validated its physical Gate A checklist on the available
X4; no new performance measurement was reported during that validation.

The Milestone 03 Gate B artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-fb517134-80x24-terminus-safe-20mhz-gate-b.bin
```

It was rebuilt from source checkpoint `fb517134`, is 5,662,768 bytes, embeds
version `1.5.0-dev-feature/knietty-terminal-fb517134` and FreeInk revision
`0ff05c6`, and has SHA-256
`e0e22ae5919a77c8256f35ef414ee7f4dd641add956aad2ca3c5fbc72b133d0c`.
The user physically validated its Gate B checklist on the available X4.

The earliest physically installed artifact retained for comparison is:

```text
/Users/rodrigomtorres/git/knietty/knietty-0217ada8-80x24-sd-safe.bin
```

It is 5,634,208 bytes with SHA-256
`fab38a4168101b139cfe37954de1425ae9fd9b737181ae07e54b6451be2aa687`.
The stabilization artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-60c30d06-80x24-windowed.bin
```

It was clean-built from `60c30d06`, is 5,650,912 bytes, embeds version
`1.5.0-dev-feature/knietty-terminal-60c30d06`, and has SHA-256
`2f8b5367669a5a9ad6fe1bf4313379839c90fac53c7d066a72a29ad1335c5647`.

`60c30d06` is now physically tested as described above. The experimental turbo
artifact is:

```text
/Users/rodrigomtorres/git/knietty/knietty-c946d9ed-80x24-turbo.bin
```

It was clean-built from `c946d9ed`, is 5,656,064 bytes, embeds version
`1.5.0-dev-feature/knietty-terminal-c946d9ed`, and has SHA-256
`ad73dd7a8def134426b1872a4e3d2c304540af8760665bc4584d600d78032062`.
The artifact uses Spleen; the Terminus and GNU Unifont profiles were
compile-validated but were not copied as release artifacts.

The current software-tested Terminus artifacts all embed
`1.5.0-dev-feature/knietty-terminal-500d757d`:

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-safe-20mhz.bin
```

This stock-driver safety baseline is 5,655,280 bytes and has SHA-256
`ab8cb750ce65fa9c64f3406db24ac79d0987193b7cc0dbde6a7c0b84c76c6b32`.

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-adaptive-20mhz.bin
```

This adaptive 20 MHz image is 5,656,160 bytes and has SHA-256
`6d24add5853067b8f37864627128f45dd3be003708645e18428c2103bfdf60fa`.

```text
/Users/rodrigomtorres/git/knietty/knietty-500d757d-80x24-terminus-adaptive-40mhz-EXPERIMENTAL.bin
```

This adaptive 40 MHz image is 5,656,160 bytes and has SHA-256
`087aa68e0b7cf84313316261a04acc9dc1a1b7bf8d714c9ec198b685cf594c02`.
It drives the SSD1677 SPI link beyond the board's normal 20 MHz setting and
must remain clearly labeled experimental. All three `500d757d` profiles have
now been flashed and qualitatively compared on the available X4. One safe
profile capture is recorded below; equivalent adaptive captures are still
missing.

## Flash/update commands

For this locked unit, do not use PlatformIO upload or esptool. Copy only the
application `.bin` artifact to the SD card and select it through CrossPoint's
normal in-application firmware update UI. The user has physically completed
that flow once from official 1.5.0 to the SD-safe knietty image.

Do not alter partitions, bootloader, secure-boot state, or eFuses.

## Recovery procedure

1. Keep the exact CrossPoint 1.4.1 binary and every known bootable knietty image
   outside the build directory.
2. If a new application update fails, do not retry destructive low-level tools;
   the current slot should remain selected.
3. Use CrossPoint's normal network OTA to restore official firmware. This route
   physically restored official 1.5.0 without the Unlocker.
4. Do not alter otadata manually, partitions, bootloader, secure-boot state, or
   eFuses.

## Performance measurements

The physically validated Gate B safe smoke capture is retained verbatim in
`results/gate-b-smoke.jsonl` (SHA-256
`b0712b58e78afc282089b4ae0413e13b36c8df5982ce4745aa6e8b8dcaa6d2d0c`).
It identifies build `fb517134`, FreeInk `0ff05c6`, an 800 x 480 display, safe
profile, 20 MHz SPI, 80 x 24 Terminus geometry, battery 70%, RSSI -57 dBm,
72,572 bytes free heap, and a 60,112-byte minimum observed heap.

The file contains one session record, 14 accepted command responses, and 13
correctly ordered PRESENTED/READY pairs with no rejection. Excluding the reset
and final clean, the four true window activations averaged 2.079 ms queued,
1.520 ms rendered, 2.716 ms transferred, 504.074 ms in the waveform, 0.726 ms
in baseline synchronization, and 506.790 ms total. The smallest one-cell case
used 0.483 ms render, 1.968 ms transfer, 504.013 ms waveform, 0.371 ms baseline,
and 505.981 ms total.

Seven larger patterns exceeded the 8 KiB window cap and used full-frame fast
fallback. They averaged 2.796 ms queued, 70.821 ms rendered, 109.539 ms
transferred, 503.650 ms in the waveform, 73.051 ms in baseline synchronization,
and 613.189 ms total. The final deliberate HALF clean took 1,839.792 ms,
including a 1,694.897 ms waveform, and is not an interactive measurement.

This controlled Gate B capture isolates the main safe-mode limit: even a one-cell
window spends about 504 ms in the SSD1677 activation/BUSY interval while all
firmware work before it takes only a few milliseconds. Windowing substantially
reduces render, transfer, and baseline work, but it does not shorten the stock
safe waveform. The initial reset's 656.022 ms queue is setup behavior and is
excluded from the interactive averages. The later baseline-v1 cadence and burst
suites quantify overlapping arrivals and latest-frame scheduling below.

The user subsequently recorded this 135-update safe-profile diagnostic on the
physical X4:

```text
Stock X4 partial / 20 MHz safe
Last 1642.8 ms; waveform 504.7 ms
Queue 775.0 ms; render 59.5 ms
Transfer 303.5 ms: plane 33.3 ms, LUT 0.0 ms, baseline 68.9 ms
Average 1537.0 ms; minimum 815.4 ms; maximum 1815.3 ms
Updates 135; window 0; fallback 135; settle 0; clean 0
Last region 800 x 472 / 47,200 bytes
```

This is a near-full-screen workload: the 47,200-byte region exceeds the current
8 KiB transient-window cap. More importantly, all 135 updates fell back. The
reported transfer subphases account for 102.2 ms, leaving 201.3 ms of the
303.5 ms transfer total unexplained by RAM traffic. That matches the driver's
fixed 200 ms post-refresh power-off delay and, together with the forced window
fallback, is strong evidence that CrossPoint's sunlight-fading fix was enabled.

The end-to-end `Last` value is also not one 1.64-second physical refresh. It is
775.0 ms waiting behind the preceding update followed by about 867.7 ms of this
update's render, transfer, and waveform. This confirms both a real 504.7 ms safe
waveform ceiling and a separate firmware scheduling/power penalty.

After disabling CrossPoint's sunlight-fading fix without reflashing, the user
repeated the safe-profile diagnostic:

```text
Stock X4 partial / 20 MHz safe
Last 1253.1 ms; waveform 503.7 ms
Queue 503.0 ms; render 85.9 ms
Transfer 160.3 ms: plane 34.9 ms, LUT 0.0 ms, baseline 125.2 ms
Average 1151.3 ms; minimum 628.6 ms; maximum 1402.2 ms
Updates 68; window 14; fallback 54; settle 0; clean 0
Last region 800 x 472 / 47,200 bytes
```

This confirms the source diagnosis. Waveform time was unchanged within 1 ms,
while the previously unexplained 201.3 ms disappeared: the new 160.3 ms
transfer total is almost exactly its 34.9 ms plane plus 125.2 ms baseline.
Windowing also resumed (`14/68` rather than `0/135`). The last frame was still a
47,200-byte near-full-screen update and therefore correctly exceeded the 8 KiB
window cap. Last latency improved by 389.7 ms and average latency by 385.7 ms;
the improvement combines removal of the power-down penalty with a shorter queue.

The 125.2 ms two-plane baseline is unexpectedly high relative to the 34.9 ms
single-plane transfer and should be measured again under a controlled workload.
Regardless, it strengthens the case for Mode 2 RAM ping-pong, which is intended
to eliminate that manual post-waveform baseline synchronization.

On the physically tested Terminus turbo image, after 50 displayed updates, the
user recorded: last 526.4 ms, waveform/BUSY 226.5 ms, transfer 164.7 ms, render
134.9 ms, average 576 ms, minimum 459 ms, and maximum 1975 ms. Those values came
from the first metrics implementation, which mixed waiting, approval,
diagnostics, first-frame, and disconnect/clean paints with terminal updates. In
particular, the 1975 ms maximum is consistent with a HALF clean and is not a
valid interactive maximum. Baseline v1 supersedes these mixed-path values for
controlled safe/adaptive comparisons.

The observed 226.5 ms BUSY interval establishes a physical upper bound of about
4.4 completed global activations per second for that older turbo waveform on
this unit, before rendering and SPI work. It is not the waveform used by the
new adaptive-40 diagnostic profile.

Baseline v1 is retained at `results/baseline-v1.md` with all eight raw JSONL
files and SHA-256 hashes. Across latency, cadence, and burst workloads, safe
window activations had a 503.223 ms median BUSY waveform and 505.213 ms median
display-call total. Safe began coalescing between the tested 600 and 400 ms
cadences; at 25 ms, 12 requests became four activations. Full-frame fallback
raised safe median display total to 609.649 ms, primarily through 35.780 ms of
plane transfer and 70.034 ms of baseline synchronization.

Adaptive-40 window activations had a 5.302 ms median BUSY waveform and 7.341 ms
median display-call total. It preserved all 12 requested activations down
through 50 ms cadence and merged one pair at 25 ms. Its fallback median was
65.427 ms, including 20.212 ms of plane transfer and 38.889 ms of baseline
synchronization. This two-image comparison changes both waveform and SPI rate,
so it cannot attribute their entire difference to 40 MHz. The electrical speed
also does not overturn the user's earlier weak-contrast/ghosting observation.
The next waveform-quality experiment should return SPI to the specified 20 MHz
and change only the volatile directional LUT duration toward approximately
100 ms. The measured small-window plane-time benefit from the current 40 MHz
comparison is only about 0.25 ms at the median, too small to justify confounding
a 100 ms waveform trial with an out-of-spec bus clock.

The nominal 100 ms / 20 MHz smoke capture is retained as
`results/wave100-e4238425-smoke.jsonl`, with analysis in
`results/wave100-e4238425.md`. It contains 13 completed refreshes and no rejected
commands. Four window updates had 100.078 ms median waveform and 102.098 ms
median display total. Eight full-frame fast fallbacks had 100.250 ms median
waveform and 207.474 ms median display total; their 106.901 ms transfer, 77.199
ms render, and 70.245 ms baseline medians explain why full-screen TUI output is
materially slower than a small typing update even with the same waveform.

The qualitative final-state pruning A/B is retained as
`results/wave100-diff-ab5d5784.md`. It falsifies redundant clear-and-repaint work
as the primary grain/cadence cause, but contains no new quantitative telemetry.

The first live read-only btop metrics capture before `knietty-0.1.0` reached
1,133 updates: 1,125 window updates, eight fallbacks, zero settle and zero clean.
Average display time was 187.139 ms; the last refresh used 120.494 ms waveform,
80.000 ms queue, 0.354 ms render, and 1.848 ms transfer. Two timed samples
advanced by 244 updates in 46 seconds and 53 updates in 10 seconds, both about
5.3 updates/s. One observed fallback covered 753 x 432 / 40,662 bytes. These
measurements isolate the two-to-three-second btop chunking before the panel:
the display kept presenting several frames per second while a single logical
TUI paint was trickling through the host's 512-byte/100-ms delivery path.

The physical `knietty-0.1.0` btop gate ran on the available X4/macOS setup with
the installed matching Rust host. Device status reported build
`knietty-0.1.0`, 80 x 24 Terminus, W100 Sustain1/no-settle, 20 MHz SPI, fading
fix off, Mode 2 off, RSSI -61 dBm, and battery 86%. The user reported that btop
looked good and updated quickly.

Across a 95-second steady-state sample, PTY/device traffic increased by
2,018,300 bytes and ended byte-for-byte equal at 3,296,681. Completed boundaries
increased by 190, exactly 2.0/s for btop's 500 ms interval, and host/device
boundary totals ended equal at 325. Display updates increased by 232; only two
additional full-frame fallbacks occurred. The final aggregate was 385 updates,
381 windowed and four fallback, with 314.579 ms average total. The last update
used 184 ms queue, 120.430 ms waveform, and 352.096 ms total. Current heap
returned to 53,400 bytes and the observed minimum stabilized at 24,600 bytes.

The same interval exposed 50 apparent boundary timeouts and 42 async-tail
updates. Every host boundary eventually reached the device. Source review then
found that the regular render path captured `renderStartedAtMs` before taking
`modelMutex`; RX could publish a later `lastQueuedAt` while render waited for
that lock. Unsigned `renderStartedAtMs - lastQueuedAt` underflowed and falsely
passed the 80 ms test. Other timeout sites already sampled current time after
loading the timestamp. `knietty-0.1.1` corrects this ordering without changing
the timeout or healthy 24 ms boundary latency.

The subsequent physical `knietty-0.1.1` sample ran btop for approximately one
minute. PTY/device totals advanced equally by 1,568,253 bytes and ended at
2,608,836; host/device boundaries advanced equally by 147 and ended at 260.
Updates advanced by 153 (about 2.55/s), all windowed, with no additional
fallback. Timeouts advanced by eight and async tails by six, versus 50 and 42
over the earlier 95-second 0.1.0 sample. Average total remained 314.737 ms;
minimum heap remained 24,880 bytes and current heap returned to 53,344 bytes.
The timestamp fix therefore passes integrity, cadence, windowing, and heap
gates while leaving a much smaller real late-boundary tail to investigate only
if it is visually consequential.

## Last known-good commit

- `f2497a77` is the current clean parent software head. It adds only the
  clang-format-21 correction and cppcheck findings discovered after publication
  to hardware-known-good `5135f049`: explicit terminal cell-array initialization
  and non-shadowing local names. Upstream CI passes formatting, cppcheck,
  177/177 native tests, all four stock firmware targets, and aggregate Test
  Status. These mechanical/static-analysis changes have not been separately
  reflashed.
- `5135f049` is the current rebased hardware-known-good knietty checkpoint and
  points to clean FreeInk checkpoint `0b9b49f`. It embeds firmware identity
  `knietty-0.1.2`, is based on current CrossPoint `develop` and FreeInk `main`,
  and passed the available X4/macOS release gate described above. The Rust host
  matrix passed 52 unit tests, two process-cleanup integration tests, formatting,
  clippy, release build, and PTY smoke; native tests passed 177/177; and the
  `knietty_async_window` firmware build passed. These clean branches supersede
  the earlier experimental histories for upstream review.
- `44f18ed3` is the current hardware-known-good knietty checkpoint and points to
  FreeInk `5594964`. It embeds firmware identity `knietty-0.1.1`, fixes saved
  Wi-Fi auto-connect and the burst-timeout timestamp race, retains the accepted
  80 ms fail-safe, and includes serialized SD progress for validation on the
  next update. The available X4/macOS gate passed automatic saved-network
  reconnect, fast btop, byte/boundary integrity, exit, and sleep/wake. Both
  commits are authored by `integerQuant`; publication to the new forks remains
  a separate explicitly approved push step.
- `61e61088` with FreeInk `9406d39` is physically correct only when built as the
  synchronized Mode-2 experiment, but that profile is timing-rejected and is
  not the product known-good. Its raw smoke capture and analysis are retained at
  `results/mode2-sync-61e61088.*`.
- `4f105b1f` with FreeInk `8ff8d51` is the rejected zero-copy Mode-2 candidate.
  Its transport/telemetry smoke completed, but stale whole-bank content returned
  during window updates. Do not use it for further suites.
- `edf80251` is the isolated TLS/pairing checkpoint. Its principal terminal,
  reconnect, commands, diagnostics, forget-all, exit, and sleep/wake gate passed
  on the available X4/macOS setup; the later per-host revoke and interrupted
  first-pair refinements still need their compact physical follow-up.
- `c07520d4` is the current host software-known-good checkpoint. It makes
  display mutations quiet and defers ordinary clean until 500 ms of PTY-output
  silence so the shell prompt is included in the cleaned panel state. This is a
  host-only fix and has not altered the physically validated firmware.
- The TLS source described above is ready for its isolated milestone commit.
  Its principal macOS/X4 physical gate passed. Per-host revoke, the visible
  forget hint, and two-phase interrupted-pair recovery pass software gates and
  need one compact physical follow-up. Packet-capture evidence remains
  deferred rather than invented.
- `61677477` is the current software-known-good runtime-control checkpoint and
  points to FreeInk `c0c059e`. It adds only status, safe HALF clean, and explicit
  polarity to an active approved v3 terminal. Formatting, the 48-test strict
  Rust matrix, 167/167 native tests, and both hardware-candidate builds pass.
  Its two images are not yet physically tested.
- `f188245f` is the current hardware-known-good parser/session-close/Rust-host
  checkpoint and points to FreeInk `c0c059e`. Its combined experience image was
  physically validated on the available X4/macOS setup for Codex scrolling,
  removal of the leaked cursor-style `q`, and graceful terminal/host exit.
- `ab5d5784` is a hardware-tested but rejected W100 candidate and points to FreeInk
  `2218b6c`. It adds allocation-free final-state dirty pruning without changing
  the waveform or SPI clock. Formatting, 38/38 host tests, 162/162 native tests,
  the experimental firmware build, and the safe firmware regression pass. Its
  SD flash and ordinary typing/btop A/B passed functionally, but it did not
  resolve progressive gray grain or btop cadence and is not a quality winner.
- `e4238425` is the nominal 100 ms / 20 MHz hardware-tested experimental
  checkpoint and points to FreeInk `2218b6c`. It passes formatting, 38/38 host
  tests, 160/160 native tests, both firmware builds, SD flash, ordinary terminal
  use, btop, and the bounded smoke gate. It is not a release-quality replacement
  for safe because the grain and TUI cadence failures above remain.
- `ae82c301` is the current firmware hardware-known-good checkpoint and points
  to FreeInk `0ff05c6`. It passes 37/37 checkpoint host tests, 160/160 native
  tests, formatting, both firmware builds, and the complete safe/adaptive
  baseline-v1 hardware campaign.
- `2880ba38` is the current host-known-good checkpoint. It passes 38/38 host
  tests and physically fixed back-to-back diagnostic rediscovery without a
  firmware change.
- `fb517134` is the previous hardware-known-good checkpoint and points to FreeInk
  `0ff05c6`. It passes 36/36 host tests, 160/160 native tests, the safe firmware
  build, ordinary v3 and forced-v2 terminal checks, and the full Gate B
  diagnostics checklist.
- `4db85157` is the earlier Milestone 01 hardware-known-good checkpoint. It
  passes 24/24 host tests, 152/152 native tests, formatting, the safe firmware
  build, and the user-confirmed Gate A checklist.
- `500d757d` is the current software- and hardware-tested knietty checkpoint and
  points to FreeInk commit `60b040f`. Host tests pass 24/24, native tests pass
  149/149, and all three firmware profiles build and boot. Safe 20 MHz is the
  preferred baseline; adaptive 40 MHz remains experimental.
- `60c30d06` is the latest physically booted terminal checkpoint. Its sleep/wake,
  icon, btop/glyph, exit, and host-disconnect behavior are known good; its
  remaining UI/latency observations are recorded above.
- `b80046b3` documents the physically tested Terminus turbo artifact and points
  to FreeInk commit `72ff720`.
- Official CrossPoint 1.5.0 is the physically tested recovery firmware.
- `33f07db7` is the built unmodified upstream baseline.

## Next concrete step

Monitor FreeInk PR #49 and CrossPoint PR #3158, address concrete maintainer
feedback, and do not merge until both repositories agree on the dependency
integration path. The CrossPoint draft is explicitly outside the current
core-reader scope; treat maintainer scope direction as the gate. Retain 80 x 24
and keep the temporary forked submodule URL until the SDK dependency has an
accepted canonical commit.

Release evidence still needed includes abrupt WLAN/power keepalive disconnect
time under TLS and an independent Linux host matrix. Exact macOS/device results
must not be generalized to Linux.

A packet capture remains release evidence to collect when a suitable
host/interface is available; do not invent that result.

Backlog: BLE keyboard input and host relay. Start it only after display latency,
the Rust host, and TLS are stable.

The detailed source anchors, implementation order, verification commands,
hardware gates, and completion criteria are in
`docs/knietty-handoff/README.md` and its numbered milestone files. Those files
are the execution authority; this list is the summary.
