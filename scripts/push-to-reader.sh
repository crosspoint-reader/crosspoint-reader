#!/bin/bash
# push-to-reader.sh — Push a file to a CrossPoint reader via HTTP upload
# Usage: ./scripts/push-to-reader.sh <file> [reader-ip] [dest-path]
#
# Examples:
#   ./scripts/push-to-reader.sh .pio/build/default/firmware.bin 192.168.0.234
#   ./scripts/push-to-reader.sh mybook.epub 192.168.0.234 /Books/chip/
#   ./scripts/push-to-reader.sh art.bmp 192.168.0.194 /sleep/

FILE="${1}"
READER_IP="${2}"
DEST_PATH="${3:-/}"

if [ -z "$FILE" ] || [ -z "$READER_IP" ]; then
  echo "Usage: $0 <file> [reader-ip] [dest-path]"
  
  echo "  dest-path defaults to / (root)"
  exit 1
fi

if [ ! -f "$FILE" ]; then
  echo "Error: file not found: $FILE"
  exit 1
fi

FILENAME=$(basename "$FILE")
echo "Pushing $FILENAME → http://$READER_IP/upload?path=$DEST_PATH"

RESULT=$(curl -s -X POST "http://$READER_IP/upload?path=$DEST_PATH" \
  -F "file=@$FILE" 2>&1)

echo "$RESULT"

# If it's a firmware upload, auto-trigger OTA flash
if [[ "$FILENAME" == "firmware.bin" ]] && echo "$RESULT" | grep -q "successfully"; then
  echo ""
  echo "Firmware uploaded. Triggering OTA flash via /api/flash..."
  curl -s -X POST "http://$READER_IP/api/flash" 2>&1
fi
