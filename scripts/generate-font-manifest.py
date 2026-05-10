#!/usr/bin/env python3
"""Generate a fonts.json manifest from a directory of .cpfont files.

Scans the input directory (flat or nested by family) for .cpfont files, reads
their binary headers to extract style metadata, and produces a JSON manifest
suitable for device-initiated font downloads.

Usage:
    python3 scripts/generate-font-manifest.py \
        --input lib/EpdFont/scripts/output \
        --base-url "https://example.com/fonts/" \
        --output dist/fonts.json

The input directory may be flat (all .cpfont files in one dir) or nested
(family subdirectories). Family names are derived from filenames using the
convention <FamilyName>_<size>.cpfont.
"""

import argparse
import json
import os
import struct
import sys
import zlib
from pathlib import Path

# Import canonical version constants from the shared file in lib/EpdFont/scripts/
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "lib" / "EpdFont" / "scripts"))
from cpfont_version import CPFONT_VERSION, FONTS_MANIFEST_VERSION

# --- .cpfont binary format constants ---
# Global header: 8s magic, H version, H flags, B styleCount, 19x reserved
GLOBAL_HEADER_FORMAT = "<8sHHB19x"
GLOBAL_HEADER_SIZE = struct.calcsize(GLOBAL_HEADER_FORMAT)  # 32 bytes

# Style TOC entry: B styleId, 3x pad, I intervalCount, I glyphCount, ...
# We only need the first byte (styleId) from each 32-byte entry.
STYLE_TOC_ENTRY_SIZE = 32
STYLE_TOC_ENTRY_FORMAT = "<B31x"

CPFONT_MAGIC = b"CPFONT\x00\x00"

STYLE_NAMES = {0: "regular", 1: "bold", 2: "italic", 3: "bolditalic"}

# Family descriptions can be loaded from the sd-fonts.yaml config
# (via --descriptions-from) or fall back to the family name.
FAMILY_DESCRIPTIONS: dict[str, str] = {}


def load_descriptions_from_yaml(yaml_path: Path) -> dict[str, str]:
    """Load family descriptions from sd-fonts.yaml config."""
    try:
        import yaml
    except ImportError:
        print("WARNING: pyyaml not installed, cannot load descriptions from YAML", file=sys.stderr)
        return {}

    with open(yaml_path) as f:
        config = yaml.safe_load(f)

    return {f["name"]: f["description"] for f in config.get("families", []) if "description" in f}


def read_cpfont_styles(filepath: Path) -> list[str]:
    """Read style names from a .cpfont file's binary header."""
    with open(filepath, "rb") as f:
        header_data = f.read(GLOBAL_HEADER_SIZE)
        if len(header_data) < GLOBAL_HEADER_SIZE:
            print(f"  WARNING: {filepath.name} too small, skipping", file=sys.stderr)
            return []

        magic, version, _flags, style_count = struct.unpack(
            GLOBAL_HEADER_FORMAT, header_data
        )

        if magic != CPFONT_MAGIC:
            print(
                f"  WARNING: {filepath.name} bad magic {magic!r}, skipping",
                file=sys.stderr,
            )
            return []

        if version != CPFONT_VERSION:
            print(
                f"  WARNING: {filepath.name} version {version} != {CPFONT_VERSION}, skipping",
                file=sys.stderr,
            )
            return []

        styles = []
        for _ in range(style_count):
            toc_data = f.read(STYLE_TOC_ENTRY_SIZE)
            if len(toc_data) < STYLE_TOC_ENTRY_SIZE:
                break
            (style_id,) = struct.unpack(STYLE_TOC_ENTRY_FORMAT, toc_data)
            name = STYLE_NAMES.get(style_id, f"unknown({style_id})")
            styles.append(name)

        return styles


def parse_filename(filename: str) -> tuple[str, str] | None:
    """Parse '<FamilyName>_<size>.cpfont' into (family, size_str).

    Returns None if the filename doesn't match the expected pattern.
    """
    if not filename.endswith(".cpfont"):
        return None
    stem = filename[: -len(".cpfont")]
    parts = stem.rsplit("_", 1)
    if len(parts) != 2:
        return None
    family, size_str = parts
    if not size_str.isdigit():
        return None
    return family, size_str


def compute_crc32(filepath: Path) -> int:
    """Compute CRC32 of a file, matching esp_rom_crc32_le(0xFFFFFFFF, ...) ^ 0xFFFFFFFF."""
    crc = 0
    with open(filepath, "rb") as f:
        for chunk in iter(lambda: f.read(65536), b""):
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def scan_cpfont_files(input_dir: Path) -> dict[str, list[Path]]:
    """Scan input directory for .cpfont files, grouped by family name.

    Handles both flat and nested directory layouts.
    """
    families: dict[str, list[Path]] = {}

    for path in sorted(input_dir.rglob("*.cpfont")):
        if not path.is_file():
            continue
        parsed = parse_filename(path.name)
        if parsed is None:
            print(f"  WARNING: skipping {path.name} (unexpected filename format)", file=sys.stderr)
            continue
        family_name = parsed[0]
        families.setdefault(family_name, []).append(path)

    return families


def build_manifest(
    families: dict[str, list[Path]], base_url: str
) -> dict:
    """Build the manifest dict from discovered font families."""
    manifest_families = []

    for family_name in sorted(families.keys()):
        files = families[family_name]

        # Read styles from the first file (all files in a family have the
        # same styles since they're generated from the same source fonts).
        styles = read_cpfont_styles(files[0]) if files else []

        # Get description
        description = FAMILY_DESCRIPTIONS.get(family_name)
        if description is None:
            print(
                f"  WARNING: no description for family '{family_name}', "
                f"consider adding one to FAMILY_DESCRIPTIONS in {__file__}",
                file=sys.stderr,
            )
            description = family_name

        file_entries = []
        for filepath in sorted(files, key=lambda p: p.name):
            file_entries.append(
                {
                    "name": filepath.name,
                    "size": filepath.stat().st_size,
                    "crc32": compute_crc32(filepath),
                }
            )

        manifest_families.append(
            {
                "name": family_name,
                "description": description,
                "styles": styles,
                "files": file_entries,
            }
        )

    return {
        "version": FONTS_MANIFEST_VERSION,
        "baseUrl": base_url,
        "families": manifest_families,
    }


def main():
    parser = argparse.ArgumentParser(
        description="Generate fonts.json manifest from .cpfont files"
    )
    parser.add_argument(
        "--input",
        required=True,
        help="Directory containing .cpfont files (flat or nested by family)",
    )
    parser.add_argument(
        "--base-url",
        required=True,
        help="URL prefix for font downloads (device concatenates baseUrl + filename)",
    )
    parser.add_argument(
        "--output",
        required=True,
        help="Output path for fonts.json",
    )
    parser.add_argument(
        "--descriptions-from",
        default=None,
        help="Path to sd-fonts.yaml to load family descriptions (default: use family name)",
    )
    args = parser.parse_args()

    input_dir = Path(args.input)
    if not input_dir.is_dir():
        print(f"ERROR: {input_dir} is not a directory", file=sys.stderr)
        sys.exit(1)

    # Ensure base URL ends with /
    base_url = args.base_url
    if not base_url.endswith("/"):
        base_url += "/"

    # Load descriptions from YAML config if provided
    global FAMILY_DESCRIPTIONS
    if args.descriptions_from:
        desc_path = Path(args.descriptions_from)
        if desc_path.exists():
            FAMILY_DESCRIPTIONS = load_descriptions_from_yaml(desc_path)
            print(f"Loaded {len(FAMILY_DESCRIPTIONS)} descriptions from {desc_path}")
        else:
            print(f"WARNING: {desc_path} not found, using family names as descriptions", file=sys.stderr)

    print(f"Scanning {input_dir} for .cpfont files...")
    families = scan_cpfont_files(input_dir)

    if not families:
        print("ERROR: no .cpfont files found", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(families)} font families:")
    for name, files in sorted(families.items()):
        print(f"  {name}: {len(files)} files")

    manifest = build_manifest(families, base_url)

    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    with open(output_path, "w") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
