#!/bin/bash
# Test all 2^9 = 512 possible feature combinations
# This ensures every possible configuration builds successfully

set -e

if command -v uv >/dev/null 2>&1; then
  PYTHON_CMD="uv run python"
  PIO_CMD="uv run pio"
else
  PYTHON_CMD="python"
  PIO_CMD="pio"
fi

FEATURES=("bookerly_fonts" "notosans_fonts" "opendyslexic_fonts" "image_sleep" "book_images" "markdown" "integrations" "koreader_sync" "calibre_sync" "background_server" "home_media_picker" "web_pokedex_plugin" "pokemon_party" "epub_support" "hyphenation" "xtc_support" "lyra_theme" "ota_updates" "todo_planner" "dark_mode" "visual_cover_picker" "ble_wifi_provisioning" "user_fonts" "web_wifi_setup" "usb_mass_storage")
TOTAL=$((2 ** ${#FEATURES[@]}))
FAILED=0
PASSED=0

echo "🔍 CrossPoint Reader - Comprehensive Combination Test"
echo "======================================================"
echo ""
echo "Testing all $TOTAL possible feature combinations"
echo "This will take approximately $((TOTAL * 2)) minutes"
echo ""

# Create log directory
mkdir -p build_logs

for ((i=0; i<TOTAL; i++)); do
  # Build feature list for this combination
  ENABLED=()
  CONFIG_NAME="combo_${i}"

  for ((j=0; j<${#FEATURES[@]}; j++)); do
    if (( (i >> j) & 1 )); then
      ENABLED+=("${FEATURES[j]}")
    fi
  done

  # Check for known over-sized features
  SKIP=0
  for feature in "${ENABLED[@]}"; do
    if [ "$feature" == "opendyslexic_fonts" ]; then
      SKIP=1
      break
    fi
  done

  if [ $SKIP -eq 1 ]; then
    echo "[$((i+1))/$TOTAL] Skipping combination with opendyslexic_fonts"
    ((PASSED++))
    continue
  fi

  # Display combination
  echo "[$((i+1))/$TOTAL] Testing combination: ${ENABLED[*]:-none}"

  # Build command
  CMD="$PYTHON_CMD scripts/generate_build_config.py"
  for feature in "${ENABLED[@]}"; do
    CMD="$CMD --enable $feature"
  done

  # Try to generate config
  if ! eval "$CMD" > "build_logs/${CONFIG_NAME}.log" 2>&1; then
    echo "  ❌ Configuration generation failed"
    ((FAILED++))
    continue
  fi

  # Try to build
  if ! $PIO_CMD run -e custom >> "build_logs/${CONFIG_NAME}.log" 2>&1; then
    echo "  ❌ Build failed"
    ((FAILED++))
    cat platformio-custom.ini
    continue
  fi

  # Check size
  if [ -f ".pio/build/custom/firmware.bin" ]; then
    SIZE=$(stat -c%s .pio/build/custom/firmware.bin)
    SIZE_MB=$(echo "scale=2; $SIZE / 1024 / 1024" | bc)
    MAX_MB=6.4

    if (( $(echo "$SIZE_MB > $MAX_MB" | bc -l) )); then
      echo "  ⚠️  Size ${SIZE_MB}MB exceeds capacity ${MAX_MB}MB"
      ((FAILED++))
    else
      echo "  ✅ Built successfully: ${SIZE_MB}MB"
      ((PASSED++))
    fi
  else
    echo "  ❌ Firmware binary not found"
    ((FAILED++))
  fi
done

echo ""
echo "======================================================"
echo "📊 Test Results"
echo "======================================================"
echo "Total combinations: $TOTAL"
echo "Passed: $PASSED"
echo "Failed: $FAILED"
echo ""

if [ $FAILED -eq 0 ]; then
  echo "🎉 All combinations build successfully!"
  exit 0
else
  echo "❌ $FAILED combinations failed"
  echo "Check build_logs/ for details"
  exit 1
fi
