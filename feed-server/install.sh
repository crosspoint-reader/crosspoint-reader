#!/bin/bash
# Install CrossPoint feed server into the OpenClaw workspace
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST_DIR="/home/laird/clawd/scripts"
SERVICE_DIR="$HOME/.config/systemd/user"

echo "Installing CrossPoint feed server..."

# Copy feed server script
cp "$SCRIPT_DIR/feed_server.py" "$DEST_DIR/crosspoint-feed-server.py"
echo "  ✓ Installed feed_server.py → $DEST_DIR/crosspoint-feed-server.py"

# Reload systemd and restart service
systemctl --user daemon-reload
systemctl --user restart crosspoint-feed
echo "  ✓ Restarted crosspoint-feed service"

echo "Done. Feed server running at http://192.168.0.83:8090/feed.xml"
