#!/usr/bin/env bash
# Decode program counters from serial (esp_backtrace_print / abort banner) to file:line.
#
# Usage:
#   scripts/addr2line_crash.sh 0x42001234 0x42005678
#   echo 'Backtrace: 0x42001234:0x4038abcd' | scripts/addr2line_crash.sh
#
# Optional: ELF=/path/to/firmware.elf pio run -e debug && scripts/addr2line_crash.sh ...

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ELF="${ELF:-$ROOT/.pio/build/default/firmware.elf}"

find_addr2line() {
  if command -v riscv32-esp-elf-addr2line >/dev/null 2>&1; then
    echo "riscv32-esp-elf-addr2line"
    return 0
  fi
  local pio="${HOME}/.platformio/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-addr2line"
  if [[ -x "$pio" ]]; then
    echo "$pio"
    return 0
  fi
  echo "riscv32-esp-elf-addr2line not found (add PlatformIO RISC-V toolchain to PATH)" >&2
  return 1
}

ADDR2LINE="$(find_addr2line)" || exit 1

if [[ ! -f "$ELF" ]]; then
  echo "ELF not found: $ELF (run: pio run -e default)" >&2
  exit 1
fi

decode_one() {
  local addr="$1"
  echo "--- $addr ---"
  "$ADDR2LINE" -e "$ELF" -f -C -p "$addr" 2>/dev/null || echo "(addr2line failed)"
}

if [[ $# -gt 0 ]]; then
  for a in "$@"; do
    decode_one "$a"
  done
else
  while IFS= read -r line || [[ -n "${line:-}" ]]; do
    mapfile -t toks < <(grep -oE '0x[0-9a-fA-F]+' <<<"$line" || true)
    for t in "${toks[@]}"; do
      decode_one "$t"
    done
  done
fi
