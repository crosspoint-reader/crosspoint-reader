#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
crate_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

cd "$crate_dir"
cargo fmt --check
cargo test
cargo clippy --all-targets -- -D warnings
cargo build --release

pty_output=$(./target/release/knietty pty-smoke \
    --cols 42 \
    --rows 21 \
    --term vt100 \
    --command 'printf "%s:%s:%s\n" "$TERM" "$COLUMNS" "$LINES"')
case "$pty_output" in
    *vt100:42:21*) ;;
    *)
        printf '%s\n' "PTY smoke returned unexpected output: $pty_output" >&2
        exit 1
        ;;
esac

printf '%s\n' "knietty Rust software matrix passed"
