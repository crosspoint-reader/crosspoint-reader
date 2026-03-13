#!/usr/bin/env python3
"""Build SD card fonts from a declarative YAML config.

Reads sd-fonts.yaml, downloads any missing source fonts, runs
fontconvert_sdcard.py in parallel for each family, and optionally
generates the fonts.json manifest.

Usage:
    # Generate fonts (output in ./output/)
    python3 build-sd-fonts.py

    # Generate fonts + manifest
    python3 build-sd-fonts.py --manifest --base-url "http://localhost:8000/"

    # Custom config / output paths
    python3 build-sd-fonts.py --config my-fonts.yaml --output-dir dist/

    # Generate only specific families
    python3 build-sd-fonts.py --only Literata,IBMPlexMono
"""

import argparse
import os
import shutil
import subprocess
import sys
import urllib.request
from concurrent.futures import ProcessPoolExecutor, as_completed
from pathlib import Path

import yaml

SCRIPT_DIR = Path(__file__).parent
FONTCONVERT = SCRIPT_DIR / "fontconvert_sdcard.py"
EPDFONTS_DIR = SCRIPT_DIR.parent  # lib/EpdFont
DEFAULT_CONFIG = SCRIPT_DIR / "sd-fonts.yaml"
DEFAULT_OUTPUT = SCRIPT_DIR / "output"
DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts"


def download_font(url: str, dest: Path) -> Path:
    """Download a font file if not already cached. Returns the local path."""
    if dest.exists():
        return dest
    dest.parent.mkdir(parents=True, exist_ok=True)
    print(f"  Downloading {dest.name}...")
    try:
        urllib.request.urlretrieve(url, dest)
    except Exception as e:
        dest.unlink(missing_ok=True)
        raise RuntimeError(f"Failed to download {url}: {e}") from e
    size_kb = dest.stat().st_size / 1024
    print(f"  Downloaded {dest.name} ({size_kb:.0f} KB)")
    return dest


def resolve_font_path(style_spec: dict, family_name: str, style_name: str) -> Path:
    """Resolve a style spec (path or url) to a local font file path."""
    if "path" in style_spec:
        resolved = EPDFONTS_DIR / style_spec["path"]
        if not resolved.exists():
            raise FileNotFoundError(f"{family_name}/{style_name}: {resolved} not found")
        return resolved

    if "url" in style_spec:
        url = style_spec["url"]
        # Derive a stable filename from the URL
        filename = url.rsplit("/", 1)[-1]
        dest = DOWNLOAD_DIR / family_name / filename
        return download_font(url, dest)

    raise ValueError(f"{family_name}/{style_name}: must have 'path' or 'url'")


def build_family(family: dict, output_base: Path) -> tuple[str, bool, str]:
    """Build a single font family. Returns (name, success, message)."""
    name = family["name"]
    output_dir = output_base / name
    output_dir.mkdir(parents=True, exist_ok=True)

    styles = family.get("styles", {})
    intervals = family["intervals"]
    sizes = ",".join(str(s) for s in family["sizes"])

    # Resolve all font file paths (downloads as needed)
    try:
        resolved_styles = {}
        for style_name, style_spec in styles.items():
            resolved_styles[style_name] = resolve_font_path(style_spec, name, style_name)
    except (FileNotFoundError, RuntimeError) as e:
        return name, False, str(e)

    # Build the fontconvert_sdcard.py command
    cmd = [sys.executable, str(FONTCONVERT)]

    multi_style = len(resolved_styles) > 1 or "regular" not in resolved_styles
    has_any_multi = any(k in resolved_styles for k in ("regular", "bold", "italic", "bolditalic"))

    if has_any_multi and len(resolved_styles) > 1:
        # Multi-style mode
        for style_name, font_path in resolved_styles.items():
            cmd.extend([f"--{style_name}", str(font_path)])
    else:
        # Single-style mode
        style_name = next(iter(resolved_styles))
        font_path = resolved_styles[style_name]
        cmd.append(str(font_path))
        cmd.extend(["--style", style_name])

    cmd.extend(["--intervals", intervals])
    cmd.extend(["--sizes", sizes])
    cmd.extend(["--name", name])
    cmd.extend(["--output-dir", str(output_dir) + "/"])

    if family.get("force_autohint", False):
        cmd.append("--force-autohint")

    # Run fontconvert_sdcard.py
    try:
        result = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=600,
        )
        if result.returncode != 0:
            return name, False, result.stderr.strip() or f"Exit code {result.returncode}"
        return name, True, ""
    except subprocess.TimeoutExpired:
        return name, False, "Timed out after 600s"
    except Exception as e:
        return name, False, str(e)


def generate_manifest(
    families_config: list[dict], output_base: Path, base_url: str, manifest_path: Path
):
    """Generate fonts.json manifest from config + built output.

    Uses the standalone generate-font-manifest.py as a subprocess so
    descriptions come from the YAML config via --descriptions-from.
    """
    manifest_script = SCRIPT_DIR.parent.parent.parent / "scripts" / "generate-font-manifest.py"
    config_path = SCRIPT_DIR / "sd-fonts.yaml"

    if not base_url.endswith("/"):
        base_url += "/"

    cmd = [
        sys.executable, str(manifest_script),
        "--input", str(output_base),
        "--base-url", base_url,
        "--output", str(manifest_path),
    ]

    if config_path.exists():
        cmd.extend(["--descriptions-from", str(config_path)])

    manifest_path.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(cmd, capture_output=True, text=True)
    if result.returncode != 0:
        print(f"ERROR: Manifest generation failed:\n{result.stderr}", file=sys.stderr)
        return
    print(result.stdout, end="")
    print(f"Manifest written: {manifest_path}")


def main():
    parser = argparse.ArgumentParser(description="Build SD card fonts from YAML config")
    parser.add_argument(
        "--config", default=str(DEFAULT_CONFIG), help="Path to font families YAML config"
    )
    parser.add_argument(
        "--output-dir", default=str(DEFAULT_OUTPUT), help="Output directory for .cpfont files"
    )
    parser.add_argument("--only", help="Comma-separated family names to build (default: all)")
    parser.add_argument("--manifest", action="store_true", help="Also generate fonts.json manifest")
    parser.add_argument("--base-url", default="", help="Base URL for manifest (required with --manifest)")
    parser.add_argument(
        "--manifest-output", default=None, help="Manifest output path (default: <output-dir>/fonts.json)"
    )
    parser.add_argument(
        "--jobs", "-j", type=int, default=None,
        help="Max parallel jobs (default: number of families)"
    )
    parser.add_argument("--clean", action="store_true", help="Clean output directory before building")
    args = parser.parse_args()

    if args.manifest and not args.base_url:
        parser.error("--base-url is required when using --manifest")

    # Load config
    config_path = Path(args.config)
    if not config_path.exists():
        print(f"ERROR: Config not found: {config_path}", file=sys.stderr)
        sys.exit(1)

    with open(config_path) as f:
        config = yaml.safe_load(f)

    families = config.get("families", [])
    if not families:
        print("ERROR: No families defined in config", file=sys.stderr)
        sys.exit(1)

    # Filter if --only specified
    if args.only:
        only_names = set(args.only.split(","))
        families = [f for f in families if f["name"] in only_names]
        missing = only_names - {f["name"] for f in families}
        if missing:
            print(f"WARNING: families not found in config: {', '.join(missing)}", file=sys.stderr)
        if not families:
            print("ERROR: no matching families after --only filter", file=sys.stderr)
            sys.exit(1)

    output_base = Path(args.output_dir)

    if args.clean and output_base.exists():
        print(f"Cleaning {output_base}...")
        shutil.rmtree(output_base)

    output_base.mkdir(parents=True, exist_ok=True)

    # Download phase (sequential — avoids hammering servers)
    print(f"\n=== Resolving {len(families)} font families ===\n")
    for family in families:
        for style_name, style_spec in family.get("styles", {}).items():
            if "url" in style_spec:
                try:
                    resolve_font_path(style_spec, family["name"], style_name)
                except Exception as e:
                    print(f"ERROR: {e}", file=sys.stderr)
                    sys.exit(1)

    # Build phase (parallel)
    max_workers = args.jobs or len(families)
    print(f"\n=== Building {len(families)} families ({max_workers} parallel jobs) ===\n")

    failed = []
    with ProcessPoolExecutor(max_workers=max_workers) as executor:
        futures = {
            executor.submit(build_family, family, output_base): family["name"]
            for family in families
        }
        for future in as_completed(futures):
            name, success, message = future.result()
            if success:
                # Count output files
                family_dir = output_base / name
                count = len(list(family_dir.glob("*.cpfont")))
                size = sum(f.stat().st_size for f in family_dir.glob("*.cpfont"))
                print(f"  OK: {name} ({count} files, {size / 1024 / 1024:.1f} MB)")
            else:
                print(f"  FAILED: {name}: {message}", file=sys.stderr)
                failed.append(name)

    # Summary
    print(f"\n=== Summary ===\n")
    total_files = len(list(output_base.rglob("*.cpfont")))
    total_size = sum(f.stat().st_size for f in output_base.rglob("*.cpfont"))
    print(f"Total: {total_files} .cpfont files ({total_size / 1024 / 1024:.1f} MB)")

    if failed:
        print(f"\nFailed families: {', '.join(failed)}", file=sys.stderr)

    # Manifest
    if args.manifest:
        manifest_path = Path(args.manifest_output) if args.manifest_output else output_base / "fonts.json"
        generate_manifest(families, output_base, args.base_url, manifest_path)

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
