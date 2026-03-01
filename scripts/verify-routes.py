#!/usr/bin/env python3
"""
verify-routes.py — CI guard for API route coverage.

Parses CrossPointWebServer.cpp and asserts that every route in REQUIRED_ROUTES
is registered. Fails with a non-zero exit code if any are missing, so this can
run in GitHub Actions (or locally) without hardware.

Usage:
    python3 scripts/verify-routes.py
    python3 scripts/verify-routes.py --verbose
"""

import re
import sys
import argparse
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
WEBSERVER_CPP = REPO_ROOT / "src" / "network" / "CrossPointWebServer.cpp"

# Every route that must exist. Add new ones here when you add a handler.
# Format: (METHOD, path)
REQUIRED_ROUTES = [
    # Core pages
    ("HTTP_GET",  "/"),
    ("HTTP_GET",  "/files"),
    ("HTTP_GET",  "/settings"),
    # File management
    ("HTTP_GET",  "/api/files"),
    ("HTTP_GET",  "/download"),
    ("HTTP_POST", "/upload"),
    ("HTTP_POST", "/mkdir"),
    ("HTTP_POST", "/rename"),
    ("HTTP_POST", "/move"),
    ("HTTP_POST", "/delete"),
    # Settings
    ("HTTP_GET",  "/api/settings"),
    ("HTTP_POST", "/api/settings"),
    # Status & diagnostics
    ("HTTP_GET",  "/api/status"),
    ("HTTP_GET",  "/api/boot-log"),
    # Feed
    ("HTTP_GET",  "/api/feed-url"),
    ("HTTP_POST", "/api/feed-url"),
    ("HTTP_POST", "/api/feed/sync"),
    ("HTTP_GET",  "/api/feed/log"),
    # Danger Zone
    ("HTTP_GET",  "/api/danger-zone/status"),
    ("HTTP_POST", "/api/flash"),
    ("HTTP_POST", "/api/reboot"),
    ("HTTP_POST", "/api/screenshot-tour"),
    ("HTTP_GET",  "/api/firmware-status"),
]

# Regex: server->on("PATH", METHOD, ...)
ROUTE_RE = re.compile(r'server->on\(\s*"([^"]+)"\s*,\s*(HTTP_\w+)')


def parse_registered_routes(cpp_path):
    text = cpp_path.read_text(encoding="utf-8")
    return set((method, path) for path, method in ROUTE_RE.findall(text))


def main():
    parser = argparse.ArgumentParser(description="Verify API route coverage")
    parser.add_argument("--verbose", "-v", action="store_true")
    args = parser.parse_args()

    if not WEBSERVER_CPP.exists():
        print("ERROR: {} not found".format(WEBSERVER_CPP), file=sys.stderr)
        sys.exit(1)

    registered = parse_registered_routes(WEBSERVER_CPP)
    required = set(REQUIRED_ROUTES)

    missing = required - registered
    extra = registered - required  # informational only

    if args.verbose:
        print("Checked {}".format(WEBSERVER_CPP.relative_to(REPO_ROOT)))
        print("  Registered routes : {}".format(len(registered)))
        print("  Required routes   : {}".format(len(required)))
        if extra:
            print("\n  Routes not in manifest (add to REQUIRED_ROUTES if intentional):")
            for method, path in sorted(extra):
                print("    {:12s} {}".format(method, path))

    if missing:
        print("\nFAIL: {} required route(s) missing from CrossPointWebServer.cpp:\n".format(len(missing)))
        for method, path in sorted(missing):
            print("  {:12s} {}".format(method, path))
        print(
            "\nThis likely means a handler was accidentally deleted.\n"
            "Either restore the route or remove it from REQUIRED_ROUTES in scripts/verify-routes.py."
        )
        sys.exit(1)

    print("OK: all {} required routes are registered.".format(len(required)))


if __name__ == "__main__":
    main()
