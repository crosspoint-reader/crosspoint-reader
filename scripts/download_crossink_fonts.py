#!/usr/bin/env python3
"""Download CrossInk noemoji font headers into lib/EpdFont/builtinFonts/"""

import urllib.request
import os
import sys

BASE_URL = "https://raw.githubusercontent.com/uxjulia/CrossInk/main/lib/EpdFont/builtinFonts/noemoji"
DEST_DIR = os.path.join(os.path.dirname(__file__), "..", "lib", "EpdFont", "builtinFonts")

FONTS  = ["charein", "lexenddeca", "bitter"]
SIZES  = ["8", "10", "12", "14", "16", "18", "20"]
STYLES = ["regular", "bold", "italic", "bolditalic"]

def main():
    os.makedirs(DEST_DIR, exist_ok=True)
    total = len(FONTS) * len(SIZES) * len(STYLES)
    done = 0
    failed = []

    for font in FONTS:
        for size in SIZES:
            for style in STYLES:
                filename = f"{font}_{size}_{style}.h"
                url  = f"{BASE_URL}/{filename}"
                dest = os.path.join(DEST_DIR, filename)

                if os.path.exists(dest):
                    print(f"  skip  {filename} (already exists)")
                    done += 1
                    continue

                try:
                    urllib.request.urlretrieve(url, dest)
                    done += 1
                    print(f"  [{done}/{total}] {filename}")
                except Exception as e:
                    print(f"  FAIL  {filename}: {e}", file=sys.stderr)
                    failed.append(filename)

    print()
    if failed:
        print(f"Failed ({len(failed)}):")
        for f in failed:
            print(f"  {f}")
        sys.exit(1)
    else:
        print(f"All {total} files downloaded to {os.path.abspath(DEST_DIR)}")

if __name__ == "__main__":
    main()
