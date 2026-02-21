#!/bin/bash
# Download Noto Sans CJK SC (Simplified Chinese) font files.
# The SC variant has broad CJK Unified Ideograph coverage and also covers
# Japanese kanji and Korean hanja.
#
# Source: https://github.com/notofonts/noto-cjk/releases/tag/Sans2.004

set -e
cd "$(dirname "$0")"

ZIP_URL="https://github.com/notofonts/noto-cjk/releases/download/Sans2.004/08_NotoSansCJKsc.zip"
ZIP_FILE="NotoSansCJKsc.zip"

if [ -f "NotoSansCJKsc-Regular.otf" ] && [ -f "NotoSansCJKsc-Bold.otf" ]; then
    echo "Font files already exist, skipping download."
    exit 0
fi

echo "Downloading Noto Sans CJK SC..."
curl -L -o "$ZIP_FILE" "$ZIP_URL"

echo "Extracting..."
unzip -o "$ZIP_FILE"

# Verify the files we need exist
if [ ! -f "NotoSansCJKsc-Regular.otf" ] || [ ! -f "NotoSansCJKsc-Bold.otf" ]; then
    echo "ERROR: Expected font files not found after extraction!"
    exit 1
fi

echo "Cleaning up zip..."
rm -f "$ZIP_FILE"

echo "Done. Font files:"
ls -lh *.otf
