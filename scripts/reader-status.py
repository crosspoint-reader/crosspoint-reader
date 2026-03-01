#!/usr/bin/env python3
"""
reader-status.py — Query a CrossPoint reader and print a human-readable summary.

Usage:
    python3 scripts/reader-status.py [--host laird|juliette]

Exits non-zero if the reader is offline.
"""

import argparse
import json
import sys
import urllib.request
import urllib.error

HOSTS = {
    "laird": "192.168.0.234",
    "juliette": "192.168.0.194",
}


def fetch_json(url, timeout=5):
    req = urllib.request.Request(url)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode())


def format_bytes(n):
    if n >= 1024 * 1024:
        return "{:.1f} MB".format(n / 1024 / 1024)
    if n >= 1024:
        return "{:.1f} KB".format(n / 1024)
    return "{} B".format(n)


def main():
    parser = argparse.ArgumentParser(description="CrossPoint reader status")
    parser.add_argument(
        "--host",
        choices=list(HOSTS.keys()),
        default="laird",
        help="Target reader (default: laird)",
    )
    args = parser.parse_args()

    ip = HOSTS[args.host]
    base = "http://{}".format(ip)

    # Check reachability via /api/status
    try:
        status = fetch_json("{}/api/status".format(base))
    except (urllib.error.URLError, OSError) as e:
        print("OFFLINE  {} ({}) — {}".format(args.host, ip, e))
        sys.exit(1)

    print("ONLINE   {} ({})".format(args.host, ip))
    print("  Version     : {}".format(status.get("version", "?")))
    print("  Build       : {}".format(status.get("build", "?")))
    print("  Uptime      : {} s".format(status.get("uptime", "?")))
    print("  Free heap   : {}".format(format_bytes(status.get("freeHeap", 0))))
    print("  WiFi mode   : {}".format(status.get("mode", "?")))
    print("  RSSI        : {} dBm".format(status.get("rssi", "?")))
    print("  Reset reason: {}".format(status.get("resetReason", "?")))

    # Danger Zone status
    try:
        dz = fetch_json("{}/api/danger-zone/status".format(base))
        enabled = dz.get("enabled", False)
        pw_set = dz.get("passwordSet", False)
        dz_label = "enabled" if enabled else "disabled"
        if enabled and not pw_set:
            dz_label += " (no password set)"
        print("  Danger Zone : {}".format(dz_label))
    except Exception:
        print("  Danger Zone : unknown")

    # Firmware staged?
    try:
        fw = fetch_json("{}/api/firmware-status".format(base))
        if fw.get("firmwareReady"):
            size_str = format_bytes(fw.get("fileSize", 0))
            ver = fw.get("version", "")
            label = "STAGED  {}".format(size_str)
            if ver:
                label += "  version {}".format(ver)
            print("  Firmware    : {}".format(label))
        else:
            print("  Firmware    : none staged")
    except Exception:
        print("  Firmware    : unknown")


if __name__ == "__main__":
    main()
