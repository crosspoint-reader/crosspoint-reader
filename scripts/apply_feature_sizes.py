#!/usr/bin/env python3
"""
Apply measured feature sizes from build_size_measurements.json to source files.

Updates hardcoded size values in:
  - scripts/generate_build_config.py  (size_kb=NNNN, base_size_mb = X.XX)
  - docs/configurator/index.html      (sizeKb: NNNN, const BASE_SIZE_MB = X.XX)

Usage:
    python scripts/apply_feature_sizes.py
    python scripts/apply_feature_sizes.py --input path/to/measurements.json
    python scripts/apply_feature_sizes.py --dry-run
"""

import argparse
import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_INPUT = REPO_ROOT / "build_size_measurements.json"
BUILD_CONFIG = REPO_ROOT / "scripts" / "generate_build_config.py"
CONFIGURATOR_HTML = REPO_ROOT / "docs" / "configurator" / "index.html"


def patch_python_feature_size(content: str, feature: str, new_size_kb: int) -> tuple[str, bool]:
    """Replace size_kb=NNNN in a Feature() constructor keyed by feature name."""
    # Match: 'feature_name': Feature(\n  ...\n  size_kb=NNNN
    pattern = re.compile(
        rf"('{re.escape(feature)}':\s*Feature\(.*?size_kb=)(-?\d+)",
        re.DOTALL,
    )
    new_content, count = pattern.subn(rf"\g<1>{new_size_kb}", content)
    return new_content, count > 0


def patch_python_base_size(content: str, new_base_mb: float) -> tuple[str, bool]:
    """Replace base_size_mb = X.XX in calculate_size()."""
    pattern = re.compile(r"(base_size_mb\s*=\s*)(\d+\.\d+)")
    new_content, count = pattern.subn(rf"\g<1>{new_base_mb:.2f}", content)
    return new_content, count > 0


def patch_html_feature_size(content: str, feature: str, new_size_kb: int) -> tuple[str, bool]:
    """Replace sizeKb: NNNN in the JS FEATURES object keyed by feature name."""
    # Match: feature_name: {\n  sizeKb: NNNN
    pattern = re.compile(
        rf"({re.escape(feature)}:\s*\{{\s*\n\s*sizeKb:\s*)(-?\d+)",
    )
    new_content, count = pattern.subn(rf"\g<1>{new_size_kb}", content)
    return new_content, count > 0


def patch_html_base_size(content: str, new_base_mb: float) -> tuple[str, bool]:
    """Replace const BASE_SIZE_MB = X.XX in the HTML."""
    pattern = re.compile(r"(const\s+BASE_SIZE_MB\s*=\s*)(\d+\.\d+)")
    new_content, count = pattern.subn(rf"\g<1>{new_base_mb:.2f}", content)
    return new_content, count > 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Apply measured feature sizes to source files"
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=DEFAULT_INPUT,
        help=f"Path to build_size_measurements.json (default: {DEFAULT_INPUT})",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would change without writing files",
    )
    args = parser.parse_args()

    # ── Load measurements ───────────────────────────────────────────
    if not args.input.exists():
        print(f"Error: measurements file not found: {args.input}")
        return 1

    with open(args.input) as f:
        data = json.load(f)

    feature_sizes: dict[str, int] = data["feature_sizes_kb"]
    base_size_mb: float = data["minimal_size_mb"]

    print(f"Loaded measurements from {args.input}")
    print(f"  base size:  {base_size_mb:.2f} MB")
    print(f"  features:   {len(feature_sizes)}")
    print()

    # ── Patch generate_build_config.py ──────────────────────────────
    py_content = BUILD_CONFIG.read_text()
    py_original = py_content
    py_changes: list[str] = []

    for feature, size_kb in feature_sizes.items():
        py_content, changed = patch_python_feature_size(py_content, feature, size_kb)
        if changed:
            py_changes.append(f"  {feature}: size_kb={size_kb}")

    py_content, changed = patch_python_base_size(py_content, base_size_mb)
    if changed:
        py_changes.append(f"  base_size_mb={base_size_mb:.2f}")

    if py_changes:
        print(f"generate_build_config.py:")
        for c in py_changes:
            print(c)
    else:
        print("generate_build_config.py: no changes needed")

    # ── Patch docs/configurator/index.html ──────────────────────────
    html_content = CONFIGURATOR_HTML.read_text()
    html_original = html_content
    html_changes: list[str] = []

    for feature, size_kb in feature_sizes.items():
        html_content, changed = patch_html_feature_size(html_content, feature, size_kb)
        if changed:
            html_changes.append(f"  {feature}: sizeKb={size_kb}")

    html_content, changed = patch_html_base_size(html_content, base_size_mb)
    if changed:
        html_changes.append(f"  BASE_SIZE_MB={base_size_mb:.2f}")

    if html_changes:
        print(f"docs/configurator/index.html:")
        for c in html_changes:
            print(c)
    else:
        print("docs/configurator/index.html: no changes needed")

    # ── Write files ─────────────────────────────────────────────────
    any_changes = (py_content != py_original) or (html_content != html_original)

    if not any_changes:
        print("\nAll sizes already up to date.")
        return 0

    if args.dry_run:
        print("\n(dry run — no files modified)")
        return 0

    if py_content != py_original:
        BUILD_CONFIG.write_text(py_content)
        print(f"\nWrote {BUILD_CONFIG}")

    if html_content != html_original:
        CONFIGURATOR_HTML.write_text(html_content)
        print(f"Wrote {CONFIGURATOR_HTML}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
