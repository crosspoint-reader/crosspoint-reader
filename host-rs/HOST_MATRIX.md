# Rust host validation matrix

Run this matrix separately on macOS and Linux. A result on one operating system
does not establish parity on the other. Record the OS version, Rust version,
knietty source revision, firmware build label, and X4 address with every run.

Daemon supervision is intentionally outside this matrix and remains in the
Linux integration backlog. The foreground `--reconnect` path is included.

## 1. Software gate

From the CrossPoint repository root:

```sh
./host-rs/scripts/check.sh
```

This runs formatting, all unit/fake-device/subprocess tests, strict Clippy, the
release build, and a real PTY geometry/environment smoke. It requires Rust 1.80
or newer and does not require the X4.

Record the environment:

```sh
uname -a
rustc --version
cargo --version
git rev-parse --short HEAD
```

## 2. Discovery and connection

Open knietty's waiting screen on the X4 before each command. Set:

```sh
knietty_bin=./host-rs/target/release/knietty
```

Confirm discovery returns exactly one X4:

```sh
"$knietty_bin" list --discovery-timeout 3
```

Start the normal v3 foreground session and approve it physically:

```sh
"$knietty_bin" --host auto --protocol 3 --verbose
```

Inside the terminal, verify printable input, Enter, arrows, scrolling, and a
full-screen TUI. Run `sleep 30` and press local Ctrl+C: the command must stop
while the Rust bridge remains alive. Press local Ctrl+\\: the bridge must exit
and the macOS/Linux terminal must immediately have normal echo and line editing.

Repeat once with the discovered IP to cover explicit-host resolution:

```sh
"$knietty_bin" --host X4_IP --protocol 3 --verbose
```

## 3. Approval and disconnect behavior

Start a connection and deny it on the X4. The host must print a denial and enter
the bounded backoff; Ctrl+\\ must exit that wait and restore the terminal.

For default disconnect, approve a normal connection and use the X4's two-press
exit. The host must print one disconnect and terminate rather than retrying
forever.

For foreground reconnect, use tmux and:

```sh
"$knietty_bin" --host auto --protocol 3 --reconnect --verbose
```

Create a visible shell marker, exit knietty on the X4, reopen it, and approve
again. The bridge must rediscover the device and return to the same tmux session.
Exit locally with Ctrl+\\ when complete.

## 4. Physical diagnostics

These tests deliberately alter the display. Each suite requires a distinct
physical approval. Wait until the X4 has returned to its waiting screen before
starting the next process.

```sh
mkdir -p results/rust-host-matrix

for suite in smoke latency cadence burst; do
  extra_args=
  case "$suite" in
    latency) extra_args="--repetitions 1" ;;
    cadence) extra_args="--settle-seconds 0.1" ;;
  esac
  "$knietty_bin" diagnose \
    --host auto \
    --suite "$suite" \
    --output "results/rust-host-matrix/$(uname -s)-$suite.jsonl" \
    --verbose \
    $extra_args
  printf '%s\n' "Wait for the X4 waiting screen before continuing."
done
```

Running the loop still requires one approval per suite; it is not unattended.
If shell argument expansion is undesirable, run each suite as a separate
command. Every command must exit zero, leave a nonempty JSONL file, finish with
a STOP response, return the X4 to the waiting screen, and preserve sleep/wake.

Also start one smoke diagnostic and do not approve it. Ctrl+C must terminate the
host with status 130 and leave no stuck approval or display-test state.

## 5. Result record

The user confirmed the complete non-daemon matrix on both target systems on
2026-08-16. The exact Linux distribution/version and toolchain strings were not
copied into this worktree, so this is recorded as user-confirmed hardware
evidence rather than reconstructed metadata. A 41-line macOS smoke capture is
retained at `results/rust-host-matrix/Darwin-smoke.jsonl`; other raw captures
were not copied into this checkout.

| Gate | macOS | Linux |
| --- | --- | --- |
| Software script | passed | passed |
| UDP discovery | passed | passed |
| v3 foreground shell/tmux | passed | passed |
| Ctrl+C reaches PTY | passed | passed |
| Ctrl+\\ restores host terminal | passed | passed |
| Explicit IP | passed | passed |
| Physical denial | passed | passed |
| Default X4 disconnect exits | passed | passed |
| `--reconnect` preserves tmux | passed | passed |
| Diagnostics smoke | passed | passed |
| Diagnostics latency | passed | passed |
| Diagnostics cadence | passed | passed |
| Diagnostics burst | passed | passed |
| Diagnostic interrupt/cleanup | passed | passed |
| Post-diagnostics sleep/wake | passed | passed |

## 6. Migration completion

The removal gate passed after:

1. every non-daemon row above passes on both macOS and Linux;
2. Rust JSONL files are accepted by the existing analysis workflow;
3. launchd/systemd foreground integration and documentation point to the Rust
   binary;
4. protocol fixtures needed by Rust remain self-contained under `host-rs/fixtures`;
5. a final repository search finds no runtime dependency on uv, pyserial,
   `knietty.py`, or `knietty_protocol.py`.

The tracked Python source, packaging, uv lock, and Python-only tests were then
removed together. Protocol fixtures are self-contained under
`host-rs/fixtures`; historical measurements remain preserved.
