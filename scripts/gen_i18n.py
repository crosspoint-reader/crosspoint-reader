#!/usr/bin/env python3
"""
Generate I18n C++ files from per-language YAML translations.

Reads YAML files from a translations directory (one file per language, named XX.yaml
where XX is the two-letter language code) and generates:
- I18nKeys.h:      Language enum (core only), StrId enum, LanguageMeta table,
                   StrIdEntry lookup table, helper functions
- I18nStrings.h:   String array declarations (core languages only)
- I18nStrings.cpp: String array definitions (core languages only) + StrIdEntry lookup

Core languages are baked into flash.  Non-core languages are loaded at runtime
from the SD card (/.crosspoint/languages/XX.yaml) and fall back to English
string-by-string for any missing entries.

Usage:
    python gen_i18n.py [<translations_dir> [<output_dir>]]

Example:
    python gen_i18n.py lib/I18n/translations lib/I18n/
"""

import sys
import os
import re
from pathlib import Path
from typing import List, Dict, Optional, Tuple

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Languages compiled directly into flash (order matters — sets enum values).
# English MUST be first (index 0) and is always the fallback.
CORE_LANGUAGES = ["EN", "DE", "FR", "ES", "PT"]

# GitHub raw base URL used by the on-device downloader
GITHUB_RAW_BASE = (
    "https://raw.githubusercontent.com/"
    "crosspoint-reader/crosspoint-reader/master/"
    "lib/I18n/translations/"
)

# ---------------------------------------------------------------------------
# YAML file reading (simple key: "value" format, no PyYAML dependency)
# ---------------------------------------------------------------------------


def _unescape_yaml_value(raw: str, filepath: str = "", line_num: int = 0) -> str:
    """Process escape sequences: \\\\ -> \\   \\" -> "   \\n -> newline"""
    result: List[str] = []
    i = 0
    while i < len(raw):
        if raw[i] == "\\" and i + 1 < len(raw):
            nxt = raw[i + 1]
            if nxt == "\\":
                result.append("\\")
            elif nxt == '"':
                result.append('"')
            elif nxt == "n":
                result.append("\n")
            else:
                raise ValueError(f"{filepath}:{line_num}: unknown escape '\\{nxt}'")
            i += 2
        else:
            result.append(raw[i])
            i += 1
    return "".join(result)


def parse_yaml_file(filepath: str) -> Dict[str, str]:
    """Parse a simple YAML file of the form:  key: "value" """
    result = {}
    with open(filepath, "r", encoding="utf-8") as f:
        for line_num, raw_line in enumerate(f, start=1):
            line = raw_line.rstrip("\n\r")
            if not line.strip():
                continue
            match = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)\s*:\s*"(.*)"$', line)
            if not match:
                raise ValueError(
                    f"{filepath}:{line_num}: bad format: {line!r}\n"
                    f'  Expected:  KEY: "value"'
                )
            key = match.group(1)
            raw_value = match.group(2)
            value = _unescape_yaml_value(raw_value, filepath, line_num)
            if key in result:
                raise ValueError(f"{filepath}:{line_num}: duplicate key '{key}'")
            result[key] = value
    return result


# ---------------------------------------------------------------------------
# Load all languages
# ---------------------------------------------------------------------------


def load_translations(
    translations_dir: str,
) -> Tuple[List[str], List[str], List[str], Dict[str, List[str]]]:
    """
    Read every XX.yaml file in *translations_dir* and return:
        language_codes   e.g. ["EN", "DE", "FR", "ES", "PT", "FI", ...]
        language_names   e.g. ["English", "Deutsch", ...]
        string_keys      ordered list of STR_* keys (from English)
        translations     {key: [value_per_language]}

    Core languages appear first (in CORE_LANGUAGES order), then non-core
    languages sorted alphabetically by code.
    """
    yaml_dir = Path(translations_dir)
    if not yaml_dir.is_dir():
        raise FileNotFoundError(f"Translations directory not found: {translations_dir}")

    yaml_files = sorted(yaml_dir.glob("*.yaml"))
    if not yaml_files:
        raise FileNotFoundError(f"No .yaml files found in {translations_dir}")

    # Parse every file, indexed by language code
    by_code: Dict[str, Dict[str, str]] = {}
    for yf in yaml_files:
        data = parse_yaml_file(str(yf))
        code = data.get("_language_code", "").upper()
        if not code:
            raise ValueError(f"{yf.name}: missing _language_code")
        # Verify filename matches code
        expected = f"{code}.yaml"
        if yf.name != expected:
            raise ValueError(
                f"{yf.name}: filename should be {expected} (code is '{code}')"
            )
        by_code[code] = data

    if "EN" not in by_code:
        raise ValueError("No EN.yaml found in translations directory")

    # Validate all core languages are present
    for c in CORE_LANGUAGES:
        if c not in by_code:
            raise ValueError(f"Core language '{c}' not found (expected {c}.yaml)")

    # Build ordered list: core first (in CORE_LANGUAGES order), then non-core sorted
    non_core = sorted(c for c in by_code if c not in CORE_LANGUAGES)
    ordered_codes = CORE_LANGUAGES + non_core

    language_codes: List[str] = []
    language_names: List[str] = []
    for code in ordered_codes:
        data = by_code[code]
        name = data.get("_language_name", "")
        if not name:
            raise ValueError(f"{code}.yaml: missing _language_name")
        language_codes.append(code)
        language_names.append(name)

    # String keys from English (order matters)
    english_data = by_code["EN"]
    string_keys = [k for k in english_data if not k.startswith("_")]
    for key in string_keys:
        if not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]*$", key):
            raise ValueError(f"Invalid C++ identifier in EN.yaml: '{key}'")

    # Build translations, filling missing keys from English
    translations: Dict[str, List[str]] = {}
    for key in string_keys:
        row: List[str] = []
        for code in ordered_codes:
            data = by_code[code]
            value = data.get(key, "")
            if not value.strip() and code != "EN":
                value = english_data[key]
                if code in CORE_LANGUAGES:
                    print(f"  INFO: '{key}' missing in {code}, using English fallback")
            row.append(value)
        translations[key] = row

    # Warn about extra keys in non-English files
    for code in ordered_codes:
        if code == "EN":
            continue
        extra = [
            k for k in by_code[code] if not k.startswith("_") and k not in english_data
        ]
        if extra:
            print(f"  WARNING: {code} has unknown keys: {', '.join(extra)}")

    core_count = len(CORE_LANGUAGES)
    non_core_count = len(non_core)
    print(
        f"Loaded {core_count} core + {non_core_count} non-core languages, "
        f"{len(string_keys)} string keys"
    )
    return language_codes, language_names, string_keys, translations


# ---------------------------------------------------------------------------
# C++ string escaping
# ---------------------------------------------------------------------------


def escape_cpp_string(s: str) -> List[str]:
    r"""
    Convert *s* into one or more C++ string literal segments.
    Non-ASCII characters are emitted as \xNN hex sequences.
    """
    if not s:
        return [""]

    s = s.replace("\n", "\\n")
    segments: List[str] = []
    current: List[str] = []

    def _flush() -> None:
        segments.append("".join(current))
        current.clear()

    i = 0
    while i < len(s):
        ch = s[i]
        if ch == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt in 'ntr"\\':
                current.append(ch + nxt)
                i += 2
            elif nxt == "x" and i + 3 < len(s):
                current.append(s[i : i + 4])
                _flush()
                i += 4
            else:
                current.append("\\\\")
                i += 1
        elif ch == '"':
            current.append('\\"')
            i += 1
        elif ord(ch) < 128:
            current.append(ch)
            i += 1
        else:
            for byte in ch.encode("utf-8"):
                current.append(f"\\x{byte:02X}")
                _flush()
            i += 1

    _flush()
    return segments


def format_cpp_string_literal(segments: List[str], indent: str = "    ") -> List[str]:
    """Format string segments as indented C++ string literal lines."""
    MAX_CONTENT_LEN = 113
    lines: List[str] = []

    for seg in segments:
        if len(seg) <= MAX_CONTENT_LEN:
            lines.append(f'{indent}"{seg}"')
            continue
        current = seg
        while len(current) > MAX_CONTENT_LEN:
            last_space = -1
            idx = 0
            while idx <= MAX_CONTENT_LEN and idx < len(current):
                if current[idx] == " ":
                    last_space = idx
                if current[idx] == "\\":
                    idx += 2
                else:
                    idx += 1
            if last_space != -1:
                split_point = last_space + 1
                lines.append(f'{indent}"{current[:split_point]}"')
                current = current[split_point:]
            else:
                cut_at = MAX_CONTENT_LEN
                if current[cut_at - 1] == "\\":
                    cut_at -= 1
                lines.append(f'{indent}"{current[:cut_at]}"')
                current = current[cut_at:]
        if current:
            lines.append(f'{indent}"{current}"')

    return lines


# ---------------------------------------------------------------------------
# Character-set computation
# ---------------------------------------------------------------------------


def compute_character_set(translations: Dict[str, List[str]], lang_index: int) -> str:
    """Return a sorted string of every unique character used in a language."""
    chars = set()
    for values in translations.values():
        for ch in values[lang_index]:
            chars.add(ord(ch))
    return "".join(chr(cp) for cp in sorted(chars))


# ---------------------------------------------------------------------------
# Code generators
# ---------------------------------------------------------------------------


def generate_keys_header(
    language_codes: List[str],
    language_names: List[str],
    string_keys: List[str],
    output_path: str,
) -> None:
    """Generate I18nKeys.h."""
    core_codes = [c for c in language_codes if c in CORE_LANGUAGES]

    lines: List[str] = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
    ]

    # --- Core Language enum (flash-baked) ---
    lines += [
        "// Core languages baked into flash.  EXT_LANG means an SD-card YAML is active.",
        "enum class Language : uint8_t {",
    ]
    for i, code in enumerate(core_codes):
        lines.append(f"  {code} = {i},")
    lines += [
        "  _CORE_COUNT,",
        "  EXT_LANG = 255,  // SD-card language active",
        "};",
        "",
    ]

    # --- LanguageMeta struct + ALL_LANGUAGES table (core only) ---
    lines += [
        "// Metadata for the core (flash-baked) languages.",
        "struct LanguageMeta {",
        '  const char* code;   // e.g. "EN"',
        '  const char* name;   // native display name, e.g. "English"',
        "};",
        "",
        f"constexpr uint8_t LANGUAGE_META_COUNT = {len(core_codes)}u;",
        "extern const LanguageMeta ALL_LANGUAGES[LANGUAGE_META_COUNT];",
        "",
    ]

    # --- StrId enum ---
    lines += [
        "// String IDs — index into the per-language string arrays.",
        "enum class StrId : uint16_t {",
    ]
    for key in string_keys:
        lines.append(f"  {key},")
    lines += [
        "  // Sentinel — must be last",
        "  _COUNT",
        "};",
        "",
    ]

    # --- StrIdEntry lookup (for runtime YAML parsing) ---
    lines += [
        "// Lookup table mapping string key names to StrId values.",
        "// Sorted by key for binary search.  Used by the SD-card language loader.",
        "struct StrIdEntry {",
        "  const char* key;",
        "  StrId       id;",
        "};",
        f"constexpr uint16_t STR_ID_LOOKUP_COUNT = static_cast<uint16_t>(StrId::_COUNT);",
        "extern const StrIdEntry STR_ID_LOOKUP[STR_ID_LOOKUP_COUNT];",
        "StrId strIdFromKey(const char* key);  // binary search",
        "",
    ]

    # --- Core string arrays (forward declarations) ---
    lines += [
        "// Flash string arrays — one per core language.",
        "namespace i18n_strings {",
    ]
    for code in core_codes:
        lines.append(f"extern const char* const STRINGS_{code}[];")
    lines += [
        "}  // namespace i18n_strings",
        "",
    ]

    # --- LANGUAGE_NAMES / CHARACTER_SETS ---
    lines += [
        "// Core language display names (indexed by Language enum).",
        "extern const char* const LANGUAGE_NAMES[];",
        "",
        "// Character sets for core languages (indexed by Language enum).",
        "extern const char* const CHARACTER_SETS[];",
        "",
    ]

    # --- getStringArray helper ---
    lines += [
        "// Return the flash string array for a core language.",
        "inline const char* const* getStringArray(Language lang) {",
        "  switch (lang) {",
    ]
    for code in core_codes:
        lines += [
            f"    case Language::{code}:",
            f"      return i18n_strings::STRINGS_{code};",
        ]
    lines += [
        "    default:",
        f"      return i18n_strings::STRINGS_EN;  // EN fallback",
        "  }",
        "}",
        "",
    ]

    # --- Convenience helper ---
    lines += [
        "// Number of core (flash) languages.",
        "constexpr uint8_t getCoreLanguageCount() {",
        "  return static_cast<uint8_t>(Language::_CORE_COUNT);",
        "}",
        "",
    ]

    _write_file(output_path, lines)


def generate_strings_header(
    language_codes: List[str],
    output_path: str,
) -> None:
    """Generate I18nStrings.h — core language array declarations only."""
    core_codes = [c for c in language_codes if c in CORE_LANGUAGES]

    lines: List[str] = [
        "#pragma once",
        "#include <string>",
        "",
        '#include "I18nKeys.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
        "namespace i18n_strings {",
        "",
    ]
    for code in core_codes:
        lines.append(f"extern const char* const STRINGS_{code}[];")
    lines += [
        "",
        "}  // namespace i18n_strings",
    ]

    _write_file(output_path, lines)


def generate_strings_cpp(
    language_codes: List[str],
    language_names: List[str],
    string_keys: List[str],
    translations: Dict[str, List[str]],
    output_path: str,
) -> None:
    """Generate I18nStrings.cpp — core arrays, StrIdEntry lookup, LANGUAGE_NAMES."""
    core_codes = [c for c in language_codes if c in CORE_LANGUAGES]

    lines: List[str] = [
        '#include "I18nStrings.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
    ]

    # ALL_LANGUAGES table (core only — runtime list built dynamically by LanguageRegistry)
    lines += [
        "// Core (flash-baked) languages only.",
        "const LanguageMeta ALL_LANGUAGES[LANGUAGE_META_COUNT] = {",
    ]
    for code in core_codes:
        idx = language_codes.index(code)
        lines.append(f'  {{"{code}", "{language_names[idx]}"}},')
    lines += ["};", ""]

    # LANGUAGE_NAMES for core languages (indexed by Language enum)
    lines += [
        "// Display names for core languages.",
        "const char* const LANGUAGE_NAMES[] = {",
    ]
    for code in core_codes:
        idx = language_codes.index(code)
        _append_string_entry(lines, language_names[idx])
    lines += ["};", ""]

    # CHARACTER_SETS for core languages
    lines += [
        "// Character sets for core languages.",
        "const char* const CHARACTER_SETS[] = {",
    ]
    for code in core_codes:
        idx = language_codes.index(code)
        charset = compute_character_set(translations, idx)
        _append_string_entry(lines, charset, comment=language_names[idx])
    lines += ["};", ""]

    # Per-core-language string arrays
    lines += ["namespace i18n_strings {", ""]
    for code in core_codes:
        lang_idx = language_codes.index(code)
        lines.append(f"const char* const STRINGS_{code}[] = {{")
        for key in string_keys:
            _append_string_entry(lines, translations[key][lang_idx])
        lines += ["};", ""]
    lines += ["}  // namespace i18n_strings", ""]

    # Compile-time size checks for core arrays
    lines.append("// Compile-time size checks")
    for code in core_codes:
        lines += [
            f"static_assert(sizeof(i18n_strings::STRINGS_{code})"
            f" / sizeof(i18n_strings::STRINGS_{code}[0]) ==",
            "                  static_cast<size_t>(StrId::_COUNT),",
            f'              "STRINGS_{code} size mismatch");',
        ]
    lines.append("")

    # STR_ID_LOOKUP table — sorted alphabetically by key for binary search
    sorted_keys = sorted(string_keys)
    lines += [
        "// Sorted lookup table: string key name -> StrId.",
        "// Used by the SD-card YAML loader (strIdFromKey).",
        "const StrIdEntry STR_ID_LOOKUP[STR_ID_LOOKUP_COUNT] = {",
    ]
    for key in sorted_keys:
        lines.append(f'  {{"{key}", StrId::{key}}},')
    lines += ["};", ""]

    # strIdFromKey — binary search
    lines += [
        "StrId strIdFromKey(const char* key) {",
        "  int lo = 0, hi = static_cast<int>(StrId::_COUNT) - 1;",
        "  while (lo <= hi) {",
        "    const int mid = lo + (hi - lo) / 2;",
        "    const int cmp = __builtin_strcmp(key, STR_ID_LOOKUP[mid].key);",
        "    if (cmp == 0) return STR_ID_LOOKUP[mid].id;",
        "    if (cmp < 0) hi = mid - 1;",
        "    else lo = mid + 1;",
        "  }",
        "  return StrId::_COUNT;  // not found",
        "}",
        "",
    ]

    _write_file(output_path, lines)


# ---------------------------------------------------------------------------
# Manifest generator
# ---------------------------------------------------------------------------


def validate_manifest(
    language_codes: List[str],
    manifest_path: str,
) -> None:
    """Warn if manifest.json is out of sync with the YAML files.

    manifest.json is a manually maintained committed file (not auto-generated).
    This check catches missing or stale entries so contributors notice early.
    """
    import json

    non_core = {c for c in language_codes if c not in CORE_LANGUAGES}

    if not os.path.exists(manifest_path):
        print(
            f"WARNING: {manifest_path} not found. "
            "Create it to allow on-device language discovery."
        )
        return

    with open(manifest_path, encoding="utf-8") as f:
        try:
            entries = json.load(f)
        except json.JSONDecodeError as e:
            print(f"WARNING: {manifest_path} is not valid JSON: {e}")
            return

    manifest_codes = {e["code"] for e in entries if "code" in e}
    missing = non_core - manifest_codes
    extra = manifest_codes - non_core

    if missing:
        print(
            f"WARNING: manifest.json is missing languages: {', '.join(sorted(missing))}"
        )
    if extra:
        print(
            f"WARNING: manifest.json has extra codes not in translations/: {', '.join(sorted(extra))}"
        )
    if not missing and not extra:
        print(
            f"OK: manifest.json is in sync ({len(manifest_codes)} non-core languages)"
        )


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _append_string_entry(lines: List[str], text: str, comment: str = "") -> None:
    segments = escape_cpp_string(text)
    formatted = format_cpp_string_literal(segments)
    suffix = f",  // {comment}" if comment else ","
    formatted[-1] += suffix
    lines.extend(formatted)


def _write_file(path: str, lines: List[str]) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
        f.write("\n")
    print(f"Generated: {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main(
    translations_dir: Optional[str] = None, output_dir: Optional[str] = None
) -> None:
    default_translations_dir = "lib/I18n/translations"
    default_output_dir = "lib/I18n/"

    if translations_dir is None or output_dir is None:
        if len(sys.argv) == 3:
            translations_dir = sys.argv[1]
            output_dir = sys.argv[2]
        else:
            translations_dir = default_translations_dir
            output_dir = default_output_dir

    if not os.path.isdir(translations_dir):
        print(f"Error: Translations directory not found: {translations_dir}")
        sys.exit(1)
    if not os.path.isdir(output_dir):
        print(f"Error: Output directory not found: {output_dir}")
        sys.exit(1)

    print(f"Reading translations from: {translations_dir}")
    print(f"Output directory: {output_dir}")
    print(f"Core languages: {', '.join(CORE_LANGUAGES)}")
    print()

    try:
        language_codes, language_names, string_keys, translations = load_translations(
            translations_dir
        )

        out = Path(output_dir)
        generate_keys_header(
            language_codes, language_names, string_keys, str(out / "I18nKeys.h")
        )
        generate_strings_header(language_codes, str(out / "I18nStrings.h"))
        generate_strings_cpp(
            language_codes,
            language_names,
            string_keys,
            translations,
            str(out / "I18nStrings.cpp"),
        )
        validate_manifest(
            language_codes,
            str(Path(translations_dir) / "manifest.json"),
        )

        print()
        print("OK: Code generation complete!")
        print(f"  Core languages ({len(CORE_LANGUAGES)}): {', '.join(CORE_LANGUAGES)}")
        non_core = [c for c in language_codes if c not in CORE_LANGUAGES]
        print(f"  Non-core languages ({len(non_core)}): {', '.join(non_core)}")
        print(f"  String keys: {len(string_keys)}")

    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)


if __name__ == "__main__":
    main()
else:
    try:
        Import("env")
        print("Running i18n generation script from PlatformIO...")
        main()
    except NameError:
        pass
