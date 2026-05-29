#!/usr/bin/env python3
"""
Provision Google OAuth credentials onto a Taskpoint device over USB-C.

Sends a "CMD:GOOGLE_CREDS {json}" command over the USB CDC serial console, which
the firmware validates and writes to /.crosspoint/google_creds.json on the SD
card. No need to remove the SD card.

The device must be awake and running (e.g. on the home screen) so its main loop
is polling the serial console.

Examples:
    # From a creds JSON file (the same file you'd otherwise drop on the SD card):
    python scripts/upload_google_creds.py --port /dev/ttyACM0 --creds-file google_creds.json

    # From individual values:
    python scripts/upload_google_creds.py --port COM7 \
        --client-id YOUR_ID.apps.googleusercontent.com \
        --client-secret YOUR_SECRET \
        --refresh-token YOUR_REFRESH_TOKEN

Requires pyserial:  pip install pyserial
"""

import argparse
import json
import sys
import time

try:
    import serial  # pyserial
except ImportError:
    sys.exit("pyserial is required: pip install pyserial")

REQUIRED_KEYS = ("client_id", "client_secret", "refresh_token")


def build_payload(args) -> dict:
    if args.creds_file:
        with open(args.creds_file, "r", encoding="utf-8") as f:
            data = json.load(f)
    else:
        data = {
            "client_id": args.client_id,
            "client_secret": args.client_secret,
            "refresh_token": args.refresh_token,
        }

    missing = [k for k in REQUIRED_KEYS if not data.get(k)]
    if missing:
        sys.exit(f"Missing required field(s): {', '.join(missing)}")

    # Keep only the three keys the firmware needs, as one compact line (the
    # firmware reads a single line up to the first newline).
    return {k: data[k] for k in REQUIRED_KEYS}


def main() -> int:
    parser = argparse.ArgumentParser(description="Upload Google credentials to a Taskpoint device over USB-C.")
    parser.add_argument("--port", required=True, help="Serial port (e.g. /dev/ttyACM0, COM7)")
    parser.add_argument("--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--creds-file", help="Path to a google_creds.json file")
    parser.add_argument("--client-id")
    parser.add_argument("--client-secret")
    parser.add_argument("--refresh-token")
    parser.add_argument("--timeout", type=float, default=5.0, help="Seconds to wait for device ack")
    args = parser.parse_args()

    if not args.creds_file and not (args.client_id and args.client_secret and args.refresh_token):
        sys.exit("Provide --creds-file OR all of --client-id, --client-secret and --refresh-token")

    payload = build_payload(args)
    line = "CMD:GOOGLE_CREDS " + json.dumps(payload, separators=(",", ":")) + "\n"

    with serial.Serial(args.port, args.baud, timeout=0.5) as ser:
        time.sleep(0.5)  # let the CDC port settle after open
        ser.reset_input_buffer()
        ser.write(line.encode("utf-8"))
        ser.flush()

        deadline = time.time() + args.timeout
        while time.time() < deadline:
            raw = ser.readline()
            if not raw:
                continue
            text = raw.decode("utf-8", errors="replace").strip()
            if not text:
                continue
            if "GOOGLE_CREDS_OK" in text:
                print("OK: credentials written to /.crosspoint/google_creds.json")
                return 0
            if "GOOGLE_CREDS_ERR" in text:
                print(f"Device rejected credentials: {text}")
                return 1

    print("Timed out waiting for device ack. Is the device awake and on the home screen?")
    return 2


if __name__ == "__main__":
    sys.exit(main())
