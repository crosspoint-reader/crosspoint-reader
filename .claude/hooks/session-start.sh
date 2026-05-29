#!/bin/bash
set -euo pipefail

# Only run in remote Claude Code on the web environments
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
  exit 0
fi

cd "${CLAUDE_PROJECT_DIR:-$(git rev-parse --show-toplevel)}"

echo "==> Installing Python dependencies..."
uv pip install --system -q -r requirements.txt

echo "==> Installing PlatformIO core..."
uv pip install --system -q -U "https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip"

echo "==> Pre-installing PlatformIO platform and library packages..."
pio pkg install --environment default 2>&1 || echo "Note: some PlatformIO packages could not be pre-installed (network or platform limits)"

echo "==> Installing clang-format (v21+)..."
if ! command -v clang-format >/dev/null 2>&1 || \
   [ "$(clang-format --version | grep -oE '[0-9]+' | head -n1)" -lt 21 ]; then
  uv pip install --system -q "clang-format>=21"
fi

echo "==> Session setup complete."
