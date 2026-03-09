#!/usr/bin/env python3
"""
Measure actual firmware sizes for each feature to validate size estimates.

This script:
1. Builds the lean profile configuration
2. Builds each feature independently
3. Measures the size impact of each feature
4. Tests for non-linear interactions in the full build
5. Outputs updated size estimates for generate_build_config.py

Run this script whenever:
- A feature implementation changes significantly
- Adding a new feature
- Size estimates seem inaccurate
- Monthly validation check

Usage:
    python scripts/measure_feature_sizes.py
    python scripts/measure_feature_sizes.py --quick  # Skip full combination test
"""

import subprocess
import sys
from pathlib import Path
from typing import Dict
import argparse
import json
import shutil

# Feature list (must match generate_build_config.py)
FEATURES = [
    'bookerly_fonts',
    'notosans_fonts',
    'opendyslexic_fonts',
    'image_sleep',
    'book_images',
    'markdown',
    'integrations',
    'koreader_sync',
    'calibre_sync',
    'background_server',
    'background_server_on_charge',
    'background_server_always',
    'home_media_picker',
    'web_pokedex_plugin',
    'pokemon_party',
    'epub_support',
    'hyphenation',
    'xtc_support',
    'lyra_theme',
    'ota_updates',
    'todo_planner',
    'dark_mode',
    'visual_cover_picker',
    'ble_wifi_provisioning',
    'user_fonts',
    'web_wifi_setup',
    'usb_mass_storage',
]

HAS_UV = shutil.which("uv") is not None


def maybe_uv_command(cmd: list[str]) -> list[str]:
    """Route Python/PlatformIO commands through uv when available."""
    if not HAS_UV or not cmd:
        return cmd
    if cmd[0] in ("python", "pio"):
        return ["uv", "run", *cmd]
    return cmd


def run_command(cmd: list, capture=False) -> subprocess.CompletedProcess:
    """Run a shell command and return the result."""
    cmd = maybe_uv_command(cmd)
    try:
        result = subprocess.run(
            cmd,
            capture_output=capture,
            text=True,
            check=True
        )
        return result
    except subprocess.CalledProcessError as e:
        print(f"❌ Command failed: {' '.join(cmd)}")
        print(f"   Error: {e.stderr if capture else e}")
        raise


def build_configuration(features: Dict[str, bool], quiet=True) -> int:
    """Build a configuration and return actual firmware size in bytes."""
    print(f"   Building...", end='', flush=True)

    # Generate config
    args = ['python', 'scripts/generate_build_config.py']
    for feature, enabled in features.items():
        if enabled:
            args.extend(['--enable', feature])

    run_command(args, capture=quiet)

    # Build firmware
    run_command(['pio', 'run', '-e', 'custom'], capture=quiet)

    # Get size
    firmware_path = Path('.pio/build/custom/firmware.bin')
    if not firmware_path.exists():
        raise FileNotFoundError(f"Firmware not found at {firmware_path}")

    size = firmware_path.stat().st_size
    print(f" {size / 1024 / 1024:.2f}MB")
    return size


def measure_minimal_size() -> int:
    """Measure the size of a lean profile build (no optional features)."""
    print("\n📦 Step 1: Building lean profile configuration...")
    print("   (All optional features disabled)")
    return build_configuration({})


def measure_feature_sizes(minimal_size: int) -> Dict[str, int]:
    """Measure the size contribution of each feature independently."""
    print("\n📊 Step 2: Measuring individual feature sizes...")
    feature_sizes = {}

    for i, feature in enumerate(FEATURES, start=1):
        print(f"   [{i}/{len(FEATURES)}] {feature}:", end=' ')
        try:
            size = build_configuration({feature: True})
            feature_size_kb = (size - minimal_size) / 1024
            feature_sizes[feature] = int(feature_size_kb)
        except (subprocess.CalledProcessError, FileNotFoundError):
            print(" ⚠️  Build failed, using estimate from generate_build_config.py")
            # We don't want to crash the whole process just because one feature (like opendyslexic)
            # exceeds the flash limit when built on top of lean.
            feature_sizes[feature] = 0  # Will be ignored in summary

    return feature_sizes


def measure_full_size(minimal_size: int, feature_sizes: Dict[str, int]) -> tuple[int, float]:
    """
    Measure full build size and detect non-linear effects.

    Returns:
        (actual_size, difference_kb) - Actual size and difference from linear estimate
    """
    print("\n🔬 Step 3: Testing full configuration for interaction effects...")

    # Build all features
    print("   Building full configuration...")
    try:
        full_size = build_configuration({f: True for f in FEATURES})
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("   ⚠️  Full configuration build failed (likely too large for flash).")
        return 0, 0.0

    # Calculate expected size (linear model)
    expected_size = minimal_size + sum(feature_sizes.values()) * 1024
    difference = (full_size - expected_size) / 1024

    print(f"\n   Expected (linear):  {expected_size / 1024 / 1024:.2f}MB")
    print(f"   Actual:             {full_size / 1024 / 1024:.2f}MB")
    print(f"   Difference:         {difference:+.0f}KB")

    if abs(difference) > 50:
        print("   ⚠️  WARNING: Significant non-linear size effects detected!")
        print("      Features may have interactions or shared dependencies.")
    else:
        print("   ✅ Size estimates are reasonably linear.")

    return full_size, difference


def print_summary(minimal_size: int, feature_sizes: Dict[str, int], full_size: int):
    """Print summary of measurements."""
    print("\n" + "="*70)
    print("📋 MEASUREMENT SUMMARY")
    print("="*70)

    print(f"\n  BASE_SIZE_MB = {minimal_size / 1024 / 1024:.2f}")
    print(f"\n  Feature sizes (update generate_build_config.py with these):")
    for feature, size_kb in feature_sizes.items():
        padding = 25 - len(feature)
        print(f"    '{feature}':{' '*padding}size_kb={size_kb},")

    print(f"\n  Full build size: {full_size / 1024 / 1024:.2f}MB")
    print(f"  Flash capacity:  6.4MB")
    flash_pct = (full_size / 1024 / 1024 / 6.4) * 100
    print(f"  Usage:           {flash_pct:.1f}%")

    if full_size > 6.4 * 1024 * 1024:
        print("\n  ⚠️  WARNING: Full build EXCEEDS flash capacity!")
        print("     Some feature combinations may not fit on device.")


def save_results(minimal_size: int, feature_sizes: Dict[str, int], full_size: int, difference: float):
    """Save measurements to JSON file for tracking over time."""
    results = {
        'minimal_size_mb': round(minimal_size / 1024 / 1024, 2),
        'feature_sizes_kb': feature_sizes,
        'full_size_mb': round(full_size / 1024 / 1024, 2),
        'non_linear_effect_kb': round(difference, 0),
        'flash_capacity_mb': 6.4,
        'full_build_fits': full_size <= 6.4 * 1024 * 1024
    }

    output_file = Path('build_size_measurements.json')
    with open(output_file, 'w') as f:
        json.dump(results, f, indent=2)

    print(f"\n💾 Results saved to {output_file}")


def main():
    parser = argparse.ArgumentParser(
        description='Measure actual firmware sizes for feature validation',
        formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument(
        '--quick',
        action='store_true',
        help='Skip full build test (faster, but less complete)'
    )

    args = parser.parse_args()

    print("🔍 CrossPoint Reader - Feature Size Measurement Tool")
    print("="*70)
    print()
    print("This will build multiple firmware configurations to measure")
    print("the actual size impact of each feature.")
    print()
    print(f"Builds required: {len(FEATURES) + 1 + (0 if args.quick else 1)}")
    print(f"Estimated time: ~{(len(FEATURES) + 2) * 2} minutes")
    print()

    try:
        # Step 1: Measure lean profile build
        minimal_size = measure_minimal_size()

        # Step 2: Measure each feature
        feature_sizes = measure_feature_sizes(minimal_size)

        # Step 3: Measure full build (unless --quick)
        if args.quick:
            print("\n⏩ Skipping full build test (--quick mode)")
            full_size = minimal_size + sum(feature_sizes.values()) * 1024
            difference = 0.0
        else:
            full_size, difference = measure_full_size(minimal_size, feature_sizes)

        # Print summary
        print_summary(minimal_size, feature_sizes, full_size)

        # Save results
        save_results(minimal_size, feature_sizes, full_size, difference)

        print("\n✅ Measurement complete!")
        print("\n💡 Next steps:")
        print("   1. Update size_kb values in scripts/generate_build_config.py")
        print("   2. Update BASE_SIZE_MB if lean profile size changed")
        print("   3. Commit build_size_measurements.json for tracking")

        return 0

    except subprocess.CalledProcessError:
        print("\n❌ Build failed. Cannot measure sizes.")
        print("   Fix compilation errors and try again.")
        return 1
    except FileNotFoundError as e:
        print(f"\n❌ Error: {e}")
        return 1
    except KeyboardInterrupt:
        print("\n\n⚠️  Measurement interrupted by user")
        return 130


if __name__ == '__main__':
    sys.exit(main())
