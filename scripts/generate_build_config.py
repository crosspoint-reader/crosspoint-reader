#!/usr/bin/env python3
"""
Generate custom PlatformIO build configuration for CrossPoint Reader.
Allows selective enabling/disabling of plugins to reduce firmware size.
"""

import argparse
import sys
from pathlib import Path
from typing import Dict, List


class Feature:
    """Represents a toggleable firmware feature."""

    def __init__(self, name: str, flag: str, size_kb: int, description: str):
        self.name = name
        self.flag = flag
        self.size_kb = size_kb
        self.description = description


# Define all toggleable features
FEATURES = {
    'bookerly_fonts': Feature(
        name='Bookerly Fonts',
        flag='ENABLE_BOOKERLY_FONTS',
        size_kb=964,
        description='12/16/18pt Bookerly family'
    ),
    'notosans_fonts': Feature(
        name='Noto Sans Fonts',
        flag='ENABLE_NOTOSANS_FONTS',
        size_kb=1098,
        description='12/16/18pt Noto Sans family'
    ),
    'opendyslexic_fonts': Feature(
        name='OpenDyslexic Font Pack',
        flag='ENABLE_OPENDYSLEXIC_FONTS',
        size_kb=2938,
        description='Optional OpenDyslexic 8/10/12/14pt family (requires Bookerly + Noto Sans)'
    ),
    'image_sleep': Feature(
        name='PNG/JPEG Sleep Images',
        flag='ENABLE_IMAGE_SLEEP',
        size_kb=0,
        description='PNG and JPEG sleep screen support (BMP always included)'
    ),
    'book_images': Feature(
        name='Book Images',
        flag='ENABLE_BOOK_IMAGES',
        size_kb=0,
        description='Inline image rendering in EPUB and Markdown readers'
    ),
    'markdown': Feature(
        name='Markdown/Obsidian',
        flag='ENABLE_MARKDOWN',
        size_kb=199,
        description='Markdown and Obsidian vault reading support'
    ),
    'integrations': Feature(
        name='Integrations Base',
        flag='ENABLE_INTEGRATIONS',
        size_kb=0,
        description='Shared runtime hooks for remote sync integrations'
    ),
    'koreader_sync': Feature(
        name='KOReader Sync',
        flag='ENABLE_KOREADER_SYNC',
        size_kb=0,
        description='Sync reading progress with KOReader'
    ),
    'calibre_sync': Feature(
        name='Calibre Sync',
        flag='ENABLE_CALIBRE_SYNC',
        size_kb=0,
        description='Calibre OPDS browser and metadata sync settings'
    ),
    'background_server': Feature(
        name='Background Server',
        flag='ENABLE_BACKGROUND_SERVER',
        size_kb=3,
        description='Background web server for file management'
    ),
    'home_media_picker': Feature(
        name='Home Media Picker',
        flag='ENABLE_HOME_MEDIA_PICKER',
        size_kb=0,
        description='Streamlined home UI with horizontal book shelf + vertical menu'
    ),
    'web_pokedex_plugin': Feature(
        name='Web Pokedex Plugin',
        flag='ENABLE_WEB_POKEDEX_PLUGIN',
        size_kb=7,
        description='Browser-side Pokemon wallpaper generator at /plugins/pokedex'
    ),
    'epub_support': Feature(
        name='EPUB Support',
        flag='ENABLE_EPUB_SUPPORT',
        size_kb=145,
        description='EPUB e-book reader with CSS and chapter navigation'
    ),
    'hyphenation': Feature(
        name='Hyphenation',
        flag='ENABLE_HYPHENATION',
        size_kb=458,
        description='Language-aware hyphenation for justified EPUB text'
    ),
    'xtc_support': Feature(
        name='XTC Support',
        flag='ENABLE_XTC_SUPPORT',
        size_kb=17,
        description='XTC format reader with chapter navigation'
    ),
    'lyra_theme': Feature(
        name='Lyra Theme',
        flag='ENABLE_LYRA_THEME',
        size_kb=13,
        description='Alternative UI theme with refined spacing and layout'
    ),
    'ota_updates': Feature(
        name='OTA Updates',
        flag='ENABLE_OTA_UPDATES',
        size_kb=1,
        description='Over-the-air firmware updates via WiFi'
    ),
    'todo_planner': Feature(
        name='Todo Planner',
        flag='ENABLE_TODO_PLANNER',
        size_kb=9,
        description='Standalone daily TODO/agenda planner with .md/.txt fallback and web quick-entry'
    ),
    'dark_mode': Feature(
        name='Dark Mode',
        flag='ENABLE_DARK_MODE',
        size_kb=0,
        description='System-wide inverted color scheme'
    ),
    'visual_cover_picker': Feature(
        name='Visual Covers',
        flag='ENABLE_VISUAL_COVER_PICKER',
        size_kb=0,
        description='Grid-based book explorer with thumbnails'
    ),
    'ble_wifi_provisioning': Feature(
        name='BLE WiFi Provisioning',
        flag='ENABLE_BLE_WIFI_PROVISIONING',
        size_kb=670,
        description='Initial WiFi setup via Bluetooth LE'
    ),
    'user_fonts': Feature(
        name='User Fonts',
        flag='ENABLE_USER_FONTS',
        size_kb=7,
        description='Load custom .ttf/.otf fonts from SD card (pre-converted)'
    ),
    'web_wifi_setup': Feature(
        name='Web WiFi Setup',
        flag='ENABLE_WEB_WIFI_SETUP',
        size_kb=3,
        description='Manage WiFi networks directly from the web interface'
    ),
    'usb_mass_storage': Feature(
        name='USB Mass Storage',
        flag='ENABLE_USB_MASS_STORAGE',
        size_kb=21,
        description='On-device prompt for USB SD card access as mass storage'
    ),
}


# Feature metadata and dependencies
class FeatureMetadata:
    """Metadata about feature implementation and dependencies."""

    def __init__(self, implemented: bool, stable: bool, requires: List[str] = None,
                 conflicts: List[str] = None, recommends: List[str] = None):
        self.implemented = implemented
        self.stable = stable
        self.requires = requires or []
        self.conflicts = conflicts or []
        self.recommends = recommends or []


FEATURE_METADATA = {
    'bookerly_fonts': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'notosans_fonts': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'opendyslexic_fonts': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['bookerly_fonts', 'notosans_fonts'],
        conflicts=[],
        recommends=[]
    ),
    'image_sleep': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'book_images': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'markdown': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'integrations': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'koreader_sync': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['integrations'],
        conflicts=[],
        recommends=[]
    ),
    'calibre_sync': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['integrations'],
        conflicts=[],
        recommends=[]
    ),
    'background_server': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'home_media_picker': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'web_pokedex_plugin': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['image_sleep'],
        conflicts=[],
        recommends=[]
    ),
    'epub_support': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'hyphenation': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['epub_support'],
        conflicts=[],
        recommends=[]
    ),
    'xtc_support': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'lyra_theme': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['home_media_picker'],
        conflicts=[],
        recommends=[]
    ),
    'ota_updates': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'todo_planner': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=['markdown']
    ),
    'dark_mode': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'visual_cover_picker': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['home_media_picker'],
        conflicts=[],
        recommends=[]
    ),
    'ble_wifi_provisioning': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['web_wifi_setup'],
        conflicts=[],
        recommends=[]
    ),
    'user_fonts': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
    'web_wifi_setup': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=['background_server'],
        conflicts=[],
        recommends=[]
    ),
    'usb_mass_storage': FeatureMetadata(
        implemented=True,
        stable=True,
        requires=[],
        conflicts=[],
        recommends=[]
    ),
}


def validate_feature_configuration(enabled_features: Dict[str, bool]) -> tuple[List[str], List[str]]:
    """
    Validate feature dependencies and conflicts.

    Returns:
        (errors, warnings) - Lists of error and warning messages
    """
    errors = []
    warnings = []

    for feature_key, enabled in enabled_features.items():
        if not enabled:
            continue

        metadata = FEATURE_METADATA.get(feature_key)
        if not metadata:
            warnings.append(f"Unknown feature: {feature_key}")
            continue

        # Check if feature is implemented
        if not metadata.implemented:
            errors.append(
                f"{FEATURES[feature_key].name} is not yet implemented. "
                f"This feature flag exists but the code is not complete. "
                f"Build will fail or feature will not work."
            )

        # Check required dependencies
        for required in metadata.requires:
            if not enabled_features.get(required, False):
                errors.append(
                    f"{FEATURES[feature_key].name} requires {FEATURES[required].name} to be enabled"
                )

        # Check conflicts
        for conflict in metadata.conflicts:
            if enabled_features.get(conflict, False):
                errors.append(
                    f"{FEATURES[feature_key].name} conflicts with {FEATURES[conflict].name}. "
                    f"These features cannot be enabled together."
                )

        # Check recommendations
        for recommended in metadata.recommends:
            if not enabled_features.get(recommended, False):
                warnings.append(
                    f"{FEATURES[feature_key].name} works best with {FEATURES[recommended].name} enabled"
                )

    return errors, warnings


def apply_dependency_defaults(enabled_features: Dict[str, bool]) -> List[str]:
    """
    Auto-enable required parent features for enabled downstream features.

    Returns:
        List of warning messages describing auto-enabled dependencies.
    """
    warnings: List[str] = []
    changed = True

    while changed:
        changed = False
        for feature_key, enabled in enabled_features.items():
            if not enabled:
                continue

            metadata = FEATURE_METADATA.get(feature_key)
            if not metadata:
                continue

            for required in metadata.requires:
                if not enabled_features.get(required, False):
                    enabled_features[required] = True
                    warnings.append(
                        f"Auto-enabled {FEATURES[required].name} because {FEATURES[feature_key].name} is enabled"
                    )
                    changed = True

    return warnings


PROFILES = {
    'lean': {
        'description': 'Core reader only (~1.7MB, maximum flash headroom)',
        'features': {},
    },
    'standard': {
        'description': 'Balanced defaults (~5.0MB, recommended)',
        'features': {
            'bookerly_fonts': True,
            'notosans_fonts': True,
            'image_sleep': True,
            'epub_support': True,
            'book_images': True,
            'hyphenation': True,
            'xtc_support': True,
            'lyra_theme': True,
            'ota_updates': True,
            'background_server': True,
            'web_wifi_setup': True,
            'home_media_picker': True,
            'dark_mode': True,
            'ble_wifi_provisioning': True,
            'user_fonts': True,
            'usb_mass_storage': True,
        },
    },
    'full': {
        'description': 'Feature-rich build (~6.0MB, fits in flash)',
        'features': {
            'bookerly_fonts': True,
            'notosans_fonts': True,
            'opendyslexic_fonts': False,  # Too large to include with other fonts
            'image_sleep': True,
            'book_images': True,
            'markdown': True,
            'integrations': True,
            'koreader_sync': True,
            'calibre_sync': True,
            'epub_support': True,
            'hyphenation': True,
            'xtc_support': True,
            'lyra_theme': True,
            'ota_updates': True,
            'todo_planner': True,
            'background_server': True,
            'home_media_picker': True,
            'web_pokedex_plugin': True,
            'dark_mode': True,
            'visual_cover_picker': True,
            'ble_wifi_provisioning': True,
            'user_fonts': True,
            'web_wifi_setup': True,
            'usb_mass_storage': True,
        },
    },
}

# Backward-compatible aliases used by old docs/workflows.
LEGACY_PROFILE_ALIASES = {
    'minimal': 'lean',
}


def empty_feature_state() -> Dict[str, bool]:
    """Return a fully-initialized feature map with all options disabled."""
    return {feature_key: False for feature_key in FEATURES.keys()}


def resolve_profile_name(profile_name: str) -> str:
    """Resolve legacy profile aliases to canonical profile names."""
    return LEGACY_PROFILE_ALIASES.get(profile_name, profile_name)


def calculate_size(enabled_features: Dict[str, bool]) -> float:
    """Calculate estimated firmware size in MB."""
    base_size_mb = 2.59  # Lean profile size baseline (measured)

    for feature_key, enabled in enabled_features.items():
        if enabled:
            base_size_mb += FEATURES[feature_key].size_kb / 1024.0

    return base_size_mb


def generate_build_flags(enabled_features: Dict[str, bool]) -> List[str]:
    """Generate PlatformIO build flags for the given feature set."""
    flags = []

    for feature_key, feature in FEATURES.items():
        value = 1 if enabled_features.get(feature_key, False) else 0
        flags.append(f'-D{feature.flag}={value}')

    return flags


def generate_platformio_ini(enabled_features: Dict[str, bool], output_path: Path, profile_name: str):
    """Generate platformio-custom.ini with custom build configuration."""

    build_flags = generate_build_flags(enabled_features)
    estimated_size = calculate_size(enabled_features)

    # Generate plugin list for comment
    enabled_list = []
    disabled_list = []
    for feature_key in FEATURES.keys():
        feature = FEATURES[feature_key]
        enabled = enabled_features.get(feature_key, False)
        if enabled:
            enabled_list.append(f'{feature.name} ({feature.size_kb}K)')
        else:
            disabled_list.append(feature.name)

    enabled_comment = '\n'.join(f'#   - {name}' for name in enabled_list) if enabled_list else '#   (none)'
    disabled_comment = '\n'.join(f'#   - {name}' for name in disabled_list) if disabled_list else '#   (none)'

    content = f"""# Custom CrossPoint Reader Build Configuration
# Generated by generate_build_config.py
#
# Selected profile: {profile_name}
# Estimated firmware size: ~{estimated_size:.1f}MB
#
# Enabled plugins:
{enabled_comment}
#
# Disabled plugins:
{disabled_comment}

[env:custom]
extends = base
build_flags =
  ${{base.build_flags}}
  -DCROSSPOINT_VERSION=\\"${{crosspoint.version}}-custom\\"
{chr(10).join(f'  {flag}' for flag in build_flags)}
"""

    output_path.write_text(content)
    print(f"Generated {output_path}")
    print(f"Estimated firmware size: ~{estimated_size:.1f}MB")
    print(f"\nEnabled plugins:")
    for name in enabled_list:
        print(f"  ✓ {name}")
    if disabled_list:
        print(f"\nDisabled plugins:")
        for name in disabled_list:
            print(f"  ✗ {name}")


def main():
    parser = argparse.ArgumentParser(
        description='Generate custom CrossPoint Reader build configuration',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  # Use a profile
  %(prog)s --profile lean
  %(prog)s --profile standard
  %(prog)s --profile full

  # Enable specific plugins
  %(prog)s --enable bookerly_fonts --enable image_sleep

  # Disable specific plugins from full profile
  %(prog)s --profile full --disable markdown

  # List available plugins
  %(prog)s --list-plugins
"""
    )

    parser.add_argument(
        '--profile',
        choices=sorted(list(PROFILES.keys()) + list(LEGACY_PROFILE_ALIASES.keys())),
        help='Use a predefined plugin profile'
    )

    parser.add_argument(
        '--preset',
        choices=sorted(list(PROFILES.keys()) + list(LEGACY_PROFILE_ALIASES.keys())),
        help='Deprecated alias for --profile'
    )

    parser.add_argument(
        '--enable',
        action='append',
        choices=list(FEATURES.keys()),
        help='Enable a specific plugin (can be used multiple times)'
    )

    parser.add_argument(
        '--disable',
        action='append',
        choices=list(FEATURES.keys()),
        help='Disable a specific plugin (can be used multiple times)'
    )

    parser.add_argument(
        '--output',
        type=Path,
        default=Path('platformio-custom.ini'),
        help='Output file path (default: platformio-custom.ini)'
    )

    parser.add_argument(
        '--list-features',
        action='store_true',
        help='List all available features and exit'
    )

    parser.add_argument(
        '--list-plugins',
        action='store_true',
        help='Alias for --list-features'
    )

    args = parser.parse_args()

    # Handle --list-features / --list-plugins
    if args.list_features or args.list_plugins:
        print("Available plugins:\n")
        for key, feature in FEATURES.items():
            print(f"  {key:20} - {feature.name}")
            print(f"  {'':20}   {feature.description}")
            print(f"  {'':20}   Size impact: ~{feature.size_kb}K")
            print()

        print("\nAvailable profiles:\n")
        for profile_name, profile_info in PROFILES.items():
            print(f"  {profile_name:10} - {profile_info['description']}")

        if LEGACY_PROFILE_ALIASES:
            print("\nLegacy aliases:\n")
            for legacy_name, canonical_name in LEGACY_PROFILE_ALIASES.items():
                print(f"  {legacy_name:10} -> {canonical_name}")

        return 0

    # Handle profile/preset compatibility.
    requested_profile = args.profile
    if args.preset:
        if requested_profile and requested_profile != args.preset:
            print("❌ Cannot pass both --profile and --preset with different values")
            return 1
        requested_profile = args.preset

    # Determine enabled features
    enabled_features = empty_feature_state()
    selected_profile = "custom"
    if requested_profile:
        canonical_profile = resolve_profile_name(requested_profile)
        if canonical_profile not in PROFILES:
            print(f"❌ Unknown profile: {requested_profile}")
            return 1
        enabled_features.update(PROFILES[canonical_profile]['features'])
        selected_profile = canonical_profile
        if requested_profile in LEGACY_PROFILE_ALIASES:
            print(f"⚠️  Profile '{requested_profile}' is deprecated; using '{canonical_profile}'")
        print(f"Using profile: {canonical_profile}")
        print(f"  {PROFILES[canonical_profile]['description']}")

    # Apply --enable flags
    if args.enable:
        for feature_key in args.enable:
            enabled_features[feature_key] = True
        if selected_profile != "custom":
            selected_profile = f"{selected_profile}+overrides"

    # Apply --disable flags
    if args.disable:
        for feature_key in args.disable:
            enabled_features[feature_key] = False
        if selected_profile != "custom":
            selected_profile = f"{selected_profile}+overrides"

    dependency_warnings = apply_dependency_defaults(enabled_features)

    # Validate configuration
    errors, warnings = validate_feature_configuration(enabled_features)
    warnings = dependency_warnings + warnings

    # Print warnings
    if warnings:
        print("\n⚠️  Warnings:")
        for warning in warnings:
            print(f"  • {warning}")
        print()

    # Print errors and exit if any
    if errors:
        print("\n❌ Configuration errors:")
        for error in errors:
            print(f"  • {error}")
        print("\nConfiguration is invalid. Please fix the errors above.")
        return 1

    # Generate configuration
    generate_platformio_ini(enabled_features, args.output, selected_profile)

    print("\nTo build: uv run pio run -e custom")

    return 0


if __name__ == '__main__':
    sys.exit(main())
