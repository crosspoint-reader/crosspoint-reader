#!/usr/bin/env python3
"""
inject_pokemon_cache.py — Stores a pre-fetched PokeAPI cache for firmware builds.

Usage:
    python scripts/inject_pokemon_cache.py <cache_file.json>
    python scripts/inject_pokemon_cache.py --clear

The cache JSON is produced by the "Export Firmware Cache" button in pokedex.html.
It maps PokeAPI URLs → stripped response objects so the firmware page works offline.

This script writes a local sidecar file next to PokedexPluginPage.html. The normal
build_html.py pre-step consumes that sidecar automatically during `pio run`, so the
source HTML stays unchanged.
"""

import json
import sys
from pathlib import Path

HTML_DIR = Path(__file__).parent.parent / "src" / "network" / "html"
CACHE_OUTPUT_PATH = HTML_DIR / "PokedexPluginPage.cache.json"


def main():
    if len(sys.argv) != 2:
        print("Usage: python scripts/inject_pokemon_cache.py <cache_file.json>")
        print("       python scripts/inject_pokemon_cache.py --clear")
        sys.exit(1)

    if sys.argv[1] == "--clear":
        if CACHE_OUTPUT_PATH.exists():
            CACHE_OUTPUT_PATH.unlink()
            print(f"Removed cached firmware sidecar: {CACHE_OUTPUT_PATH}")
        else:
            print(f"No cache sidecar present: {CACHE_OUTPUT_PATH}")
        return

    cache_path = Path(sys.argv[1])
    if not cache_path.exists():
        print(f"Error: cache file not found: {cache_path}")
        sys.exit(1)

    with open(cache_path, "r", encoding="utf-8") as f:
        cache_data = json.load(f)

    with open(CACHE_OUTPUT_PATH, "w", encoding="utf-8") as output_file:
        json.dump(cache_data, output_file, separators=(",", ":"), ensure_ascii=False)

    cache_kb = CACHE_OUTPUT_PATH.stat().st_size / 1024
    print(f"Stored {len(cache_data)} cache entries in {CACHE_OUTPUT_PATH}")
    print(f"  Sidecar size: {cache_kb:.1f} KB")
    print("Run `pio run` to rebuild the firmware with the baked cache.")


if __name__ == "__main__":
    main()
