#!/usr/bin/env python3
"""
Compare a language's translation YAML against english.yaml and report
missing / extra / untranslated (same as English) keys.

Usage:
    python scripts/check_translation.py [language] [translations_dir]

Examples:
    python scripts/check_translation.py german
    python scripts/check_translation.py lib/I18n/translations/german.yaml
    python scripts/check_translation.py german lib/I18n/translations
"""

import re
import sys
from pathlib import Path
from typing import Dict


def parse_yaml_file(filepath: Path) -> Dict[str, str]:
    """Parse the simple `KEY: "value"` YAML subset used by lib/I18n/translations."""
    result: Dict[str, str] = {}
    with filepath.open("r", encoding="utf-8") as f:
        for line_num, raw_line in enumerate(f, start=1):
            line = raw_line.rstrip("\n\r")
            if not line.strip():
                continue

            match = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*"(.*)"$', line)
            if not match:
                raise ValueError(f"{filepath}:{line_num}: bad format: {line!r}")

            key, raw_value = match.group(1), match.group(2)
            if key in result:
                raise ValueError(f"{filepath}:{line_num}: duplicate key '{key}'")
            result[key] = raw_value
    return result


def resolve_target(arg: str, translations_dir: Path) -> Path:
    candidate = Path(arg)
    if candidate.is_file():
        return candidate
    candidate = translations_dir / f"{arg}.yaml"
    if candidate.is_file():
        return candidate
    raise FileNotFoundError(f"Could not find translation file for '{arg}'")


def main() -> int:
    args = sys.argv[1:]
    if not args:
        print("Usage: python scripts/check_translation.py <language> [translations_dir]")
        return 1

    lang_arg = args[0]
    translations_dir = Path(args[1]) if len(args) > 1 else Path("lib/I18n/translations")

    english_path = translations_dir / "english.yaml"
    target_path = resolve_target(lang_arg, translations_dir)

    english = parse_yaml_file(english_path)
    target = parse_yaml_file(target_path)

    english_keys = {k for k in english if not k.startswith("_")}
    target_keys = {k for k in target if not k.startswith("_")}

    missing = sorted(english_keys - target_keys)
    extra = sorted(target_keys - english_keys)
    untranslated = sorted(
        k for k in (english_keys & target_keys) if target[k] == english[k] and english[k].strip()
    )

    lang_name = target.get("_language_name", target_path.stem)
    print(f"Comparing {target_path.name} ({lang_name}) against {english_path.name}\n")

    if missing:
        print(f"Missing ({len(missing)}) — present in English, absent from {lang_name}:")
        for key in missing:
            print(f'  {key}: "{english[key]}"')
        print()
    else:
        print("No missing keys.\n")

    if untranslated:
        print(f"Untranslated ({len(untranslated)}) — identical to English, likely not yet translated:")
        for key in untranslated:
            print(f'  {key}: "{english[key]}"')
        print()

    if extra:
        print(f"Extra ({len(extra)}) — present in {lang_name}, not in English (unused/stale):")
        for key in extra:
            print(f'  {key}: "{target[key]}"')
        print()

    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
