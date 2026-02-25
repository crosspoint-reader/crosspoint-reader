#!/usr/bin/env python3
"""
Generate I18n C++ files from per-language YAML translations.

Reads YAML files from a translations directory (one file per language) and generates:
- I18nKeys.h:     Language enum, StrId enum, helper functions
- I18nStrings.h:  String array declarations
- I18nStrings.cpp: String array definitions with all translations

Each YAML file must contain:
  _language_name: "Native Name"     (e.g. "Español")
  _language_code: "ENUM_NAME"       (e.g. "ES")
  STR_KEY: "translation text"

The English file is the reference. Missing keys in other languages are
automatically filled from English, with a warning.

Usage:
    python gen_i18n.py <translations_dir> <output_dir>

Example:
    python gen_i18n.py lib/I18n/translations lib/I18n/
"""

import sys
import os
import re
from pathlib import Path
from typing import List, Dict, Tuple


# ---------------------------------------------------------------------------
# YAML file reading (simple key: "value" format, no PyYAML dependency)
# ---------------------------------------------------------------------------

def _unescape_yaml_value(raw: str, filepath: str = "", line_num: int = 0) -> str:
    """
    Process escape sequences in a YAML value string.

    Recognized escapes:  \\\\  →  \\       \\"  →  "       \\n  →  newline
    """
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
                raise ValueError(
                    f"{filepath}:{line_num}: unknown escape '\\{nxt}'"
                )
            i += 2
        else:
            result.append(raw[i])
            i += 1
    return "".join(result)


def parse_yaml_file(filepath: str) -> Dict[str, str]:
    """
    Parse a simple YAML file of the form:
        key: "value"

    Only supports flat key-value pairs with quoted string values.
    Aborts on formatting errors.
    """
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

            # Un-escape: process character by character to handle
            # \\, \", and \n sequences correctly
            value = _unescape_yaml_value(raw_value, filepath, line_num)

            if key in result:
                raise ValueError(f"{filepath}:{line_num}: duplicate key '{key}'")

            result[key] = value

    return result


# ---------------------------------------------------------------------------
# Load all languages from a directory of YAML files
# ---------------------------------------------------------------------------

def load_translations(
    translations_dir: str,
) -> Tuple[List[str], List[str], List[str], Dict[str, List[str]]]:
    """
    Read every YAML file in *translations_dir* and return:
        language_codes   e.g. ["EN", "ES", ...]
        language_names   e.g. ["English", "Español", ...]
        string_keys      ordered list of STR_* keys (from English)
        translations     {key: [translation_per_language]}

    English is always first;
    """
    yaml_dir = Path(translations_dir)
    if not yaml_dir.is_dir():
        raise FileNotFoundError(f"Translations directory not found: {translations_dir}")

    yaml_files = sorted(yaml_dir.glob("*.yaml"))
    if not yaml_files:
        raise FileNotFoundError(f"No .yaml files found in {translations_dir}")

    # Parse every file
    parsed: Dict[str, Dict[str, str]] = {}
    for yf in yaml_files:
        parsed[yf.name] = parse_yaml_file(str(yf))

    # Identify the English file (must exist)
    english_file = None
    for name, data in parsed.items():
        if data.get("_language_code", "").upper() == "EN":
            english_file = name
            break

    if english_file is None:
        raise ValueError("No YAML file with _language_code: EN found")

    # Order: English first, then by _order metadata (falls back to filename)
    def sort_key(fname: str) -> Tuple[int, int, str]:
        """English always first (0), then by _order, then by filename."""
        if fname == english_file:
            return (0, 0, fname)
        order = parsed[fname].get("_order", "999")
        try:
            order_int = int(order)
        except ValueError:
            order_int = 999
        return (1, order_int, fname)

    ordered_files = sorted(parsed, key=sort_key)

    # Extract metadata
    language_codes: List[str] = []
    language_names: List[str] = []
    for fname in ordered_files:
        data = parsed[fname]
        code = data.get("_language_code")
        name = data.get("_language_name")
        if not code or not name:
            raise ValueError(f"{fname}: missing _language_code or _language_name")
        language_codes.append(code)
        language_names.append(name)

    # String keys come from English (order matters)
    english_data = parsed[english_file]
    string_keys = [k for k in english_data if not k.startswith("_")]

    # Validate all keys are valid C++ identifiers
    for key in string_keys:
        if not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]*$", key):
            raise ValueError(f"Invalid C++ identifier in English file: '{key}'")

    # Build translations dict, filling missing keys from English
    translations: Dict[str, List[str]] = {}
    for key in string_keys:
        row: List[str] = []
        for fname in ordered_files:
            data = parsed[fname]
            value = data.get(key, "")
            if not value.strip() and fname != english_file:
                value = english_data[key]
                lang_code = parsed[fname].get("_language_code", fname)
                print(f"  INFO: '{key}' missing in {lang_code}, using English fallback")
            row.append(value)
        translations[key] = row

    # Warn about extra keys in non-English files
    for fname in ordered_files:
        if fname == english_file:
            continue
        data = parsed[fname]
        extra = [k for k in data if not k.startswith("_") and k not in english_data]
        if extra:
            lang_code = data.get("_language_code", fname)
            print(f"  WARNING: {lang_code} has keys not in English: {', '.join(extra)}")

    print(f"Loaded {len(language_codes)} languages, {len(string_keys)} string keys")
    return language_codes, language_names, string_keys, translations


# ---------------------------------------------------------------------------
# C++ string escaping
# ---------------------------------------------------------------------------

LANG_ABBREVIATIONS = {
    "english": "EN",
    "español": "ES", "espanol": "ES",
    "italiano": "IT",
    "svenska": "SV",
    "français": "FR", "francais": "FR",
    "deutsch": "DE", "german": "DE",
    "polski": "PL",
    "português": "PT", "portugues": "PT", "português (brasil)": "PO",
    "中文": "ZH", "chinese": "ZH",
    "日本語": "JA", "japanese": "JA",
    "한국어": "KO", "korean": "KO",
    "русский": "RU", "russian": "RU",
    "العربية": "AR", "arabic": "AR",
    "עברית": "HE", "hebrew": "HE",
    "فارسی": "FA", "persian": "FA",
    "čeština": "CS",
}


def get_lang_abbreviation(lang_code: str, lang_name: str) -> str:
    """Return a 2-letter abbreviation for a language."""
    lower = lang_name.lower()
    if lower in LANG_ABBREVIATIONS:
        return LANG_ABBREVIATIONS[lower]
    return lang_code[:2].upper()


def escape_cpp_string(s: str) -> List[str]:
    r"""
    Convert *s* into one or more C++ string literal segments.

    Non-ASCII characters are emitted as \xNN hex sequences. After each
    hex escape a new segment is started so the compiler doesn't merge
    subsequent hex digits into the escape.

    Returns a list of string segments (without quotes). For simple ASCII
    strings this is a single-element list.
    """
    if not s:
        return [""]

    s = s.replace("\n", "\\n")

    # Build a flat list of "tokens", where each token is either a regular
    # character sequence or a hex escape.  A segment break happens after
    # every hex escape.
    segments: List[str] = []
    current: List[str] = []
    i = 0

    def _flush() -> None:
        segments.append("".join(current))
        current.clear()

    while i < len(s):
        ch = s[i]

        if ch == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt in "ntr\"\\":
                current.append(ch + nxt)
                i += 2
            elif nxt == "x" and i + 3 < len(s):
                current.append(s[i : i + 4])
                _flush()                       # segment break after hex
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
                _flush()                       # segment break after hex
            i += 1

    # Flush remaining content
    _flush()

    return segments


def format_cpp_string_literal(segments: List[str], indent: str = "    ") -> List[str]:
    """
    Format string segments (from escape_cpp_string) as indented C++ string
    literal lines, each wrapped in quotes.
    Also wraps long segments to respect ~120 column limit.
    """
    # Effective limit for content: 120 - 4 (indent) - 2 (quotes) - 1 (comma/safety) = 113
    # Using 113 to match clang-format exactly (120 - 4 - 2 - 1)
    MAX_CONTENT_LEN = 113

    lines: List[str] = []

    for seg in segments:
        # Short segment (e.g. hex escape or short text)
        if len(seg) <= MAX_CONTENT_LEN:
            lines.append(f'{indent}"{seg}"')
            continue

        # Long segment - wrap it
        current = seg
        while len(current) > MAX_CONTENT_LEN:
            # Find best split point
            # Scan forward to find last space <= MAX_CONTENT_LEN
            last_space = -1
            idx = 0
            while idx <= MAX_CONTENT_LEN and idx < len(current):
                if current[idx] == ' ':
                    last_space = idx

                # Handle escapes to step correctly
                if current[idx] == '\\':
                    idx += 2
                else:
                    idx += 1

            # If we found a space, split after it
            if last_space != -1:
                # Include the space in the first line
                split_point = last_space + 1
                lines.append(f'{indent}"{current[:split_point]}"')
                current = current[split_point:]
            else:
                # No space, forced break at MAX_CONTENT_LEN (or slightly less)
                cut_at = MAX_CONTENT_LEN
                # Don't cut in the middle of an escape sequence
                if current[cut_at - 1] == '\\':
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
    languages: List[str],
    language_names: List[str],
    string_keys: List[str],
    output_path: str,
) -> None:
    """Generate I18nKeys.h."""
    lines: List[str] = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
        "// Forward declaration for string arrays",
        "namespace i18n_strings {",
    ]

    for code, name in zip(languages, language_names):
        abbrev = get_lang_abbreviation(code, name)
        lines.append(f"extern const char* const STRINGS_{abbrev}[];")

    lines.append("}  // namespace i18n_strings")
    lines.append("")

    # Language enum
    lines.append("// Language enum")
    lines.append("enum class Language : uint8_t {")
    for i, lang in enumerate(languages):
        lines.append(f"  {lang} = {i},")
    lines.append("  _COUNT")
    lines.append("};")
    lines.append("")

    # Extern declarations
    lines.append("// Language display names (defined in I18nStrings.cpp)")
    lines.append("extern const char* const LANGUAGE_NAMES[];")
    lines.append("")
    lines.append("// Character sets for each language (defined in I18nStrings.cpp)")
    lines.append("extern const char* const CHARACTER_SETS[];")
    lines.append("")

    # StrId enum
    lines.append("// String IDs")
    lines.append("enum class StrId : uint16_t {")
    for key in string_keys:
        lines.append(f"  {key},")
    lines.append("  // Sentinel - must be last")
    lines.append("  _COUNT")
    lines.append("};")
    lines.append("")

    # getStringArray helper
    lines.append("// Helper function to get string array for a language")
    lines.append("inline const char* const* getStringArray(Language lang) {")
    lines.append("  switch (lang) {")
    for code, name in zip(languages, language_names):
        abbrev = get_lang_abbreviation(code, name)
        lines.append(f"    case Language::{code}:")
        lines.append(f"      return i18n_strings::STRINGS_{abbrev};")
    first_abbrev = get_lang_abbreviation(languages[0], language_names[0])
    lines.append("    default:")
    lines.append(f"      return i18n_strings::STRINGS_{first_abbrev};")
    lines.append("  }")
    lines.append("}")
    lines.append("")

    # getLanguageCount helper (single line to match checked-in format)
    lines.append("// Helper function to get language count")
    lines.append(
        "constexpr uint8_t getLanguageCount() "
        "{ return static_cast<uint8_t>(Language::_COUNT); }"
    )
    lines.append("")

    # Sorted language indices for display order
    # (English first, then by language code alphabetically)
    english_idx = languages.index("EN")
    rest = sorted(
        (i for i in range(len(languages)) if i != english_idx),
        key=lambda i: languages[i],
    )
    sorted_indices = [english_idx] + rest
    comment_names = ", ".join(language_names[i] for i in sorted_indices)
    lines.append("// Sorted language indices by code (auto-generated by gen_i18n.py)")
    lines.append(f"// Order: {comment_names}")
    lines.append(
        "constexpr uint8_t SORTED_LANGUAGE_INDICES[] = {"
        f"{', '.join(str(i) for i in sorted_indices)}"
        "};"
    )
    lines.append("")
    lines.append(
        "static_assert(sizeof(SORTED_LANGUAGE_INDICES) / sizeof(SORTED_LANGUAGE_INDICES[0]) == getLanguageCount(),"
    )
    lines.append(
        '              "SORTED_LANGUAGE_INDICES size mismatch");'
    )

    _write_file(output_path, lines)


def generate_strings_header(
    languages: List[str],
    language_names: List[str],
    output_path: str,
) -> None:
    """Generate I18nStrings.h."""
    lines: List[str] = [
        "#pragma once",
        '#include <string>',
        "",
        '#include "I18nKeys.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
        "namespace i18n_strings {",
        "",
    ]

    for code, name in zip(languages, language_names):
        abbrev = get_lang_abbreviation(code, name)
        lines.append(f"extern const char* const STRINGS_{abbrev}[];")

    lines.append("")
    lines.append("}  // namespace i18n_strings")

    _write_file(output_path, lines)


def generate_strings_cpp(
    languages: List[str],
    language_names: List[str],
    string_keys: List[str],
    translations: Dict[str, List[str]],
    output_path: str,
) -> None:
    """Generate I18nStrings.cpp."""
    lines: List[str] = [
        '#include "I18nStrings.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "",
    ]

    # LANGUAGE_NAMES array
    lines.append("// Language display names")
    lines.append("const char* const LANGUAGE_NAMES[] = {")
    for name in language_names:
        _append_string_entry(lines, name)
    lines.append("};")
    lines.append("")

    # CHARACTER_SETS array
    lines.append("// Character sets for each language")
    lines.append("const char* const CHARACTER_SETS[] = {")
    for lang_idx, name in enumerate(language_names):
        charset = compute_character_set(translations, lang_idx)
        _append_string_entry(lines, charset, comment=name)
    lines.append("};")
    lines.append("")

    # Per-language string arrays
    lines.append("namespace i18n_strings {")
    lines.append("")

    for lang_idx, (code, name) in enumerate(zip(languages, language_names)):
        abbrev = get_lang_abbreviation(code, name)
        lines.append(f"const char* const STRINGS_{abbrev}[] = {{")

        for key in string_keys:
            text = translations[key][lang_idx]
            _append_string_entry(lines, text)

        lines.append("};")
        lines.append("")

    lines.append("}  // namespace i18n_strings")
    lines.append("")

    # Compile-time size checks
    lines.append("// Compile-time validation of array sizes")
    for code, name in zip(languages, language_names):
        abbrev = get_lang_abbreviation(code, name)
        lines.append(
            f"static_assert(sizeof(i18n_strings::STRINGS_{abbrev}) "
            f"/ sizeof(i18n_strings::STRINGS_{abbrev}[0]) =="
        )
        lines.append("                  static_cast<size_t>(StrId::_COUNT),")
        lines.append(f'              "STRINGS_{abbrev} size mismatch");')

    _write_file(output_path, lines)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _append_string_entry(
    lines: List[str], text: str, comment: str = ""
) -> None:
    """Escape *text*, format as indented C++ lines, append comma (and optional comment)."""
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

def main(translations_dir=None, output_dir=None) -> None:
    # Default paths (relative to project root)
    default_translations_dir = "lib/I18n/translations"
    default_output_dir = "lib/I18n/"

    if translations_dir is None or output_dir is None:
        if len(sys.argv) == 3:
            translations_dir = sys.argv[1]
            output_dir = sys.argv[2]
        else:
            # Default for no arguments or weird arguments (e.g. SCons)
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
    print()

    try:
        languages, language_names, string_keys, translations = load_translations(
            translations_dir
        )

        out = Path(output_dir)
        generate_keys_header(languages, language_names, string_keys, str(out / "I18nKeys.h"))
        generate_strings_header(languages, language_names, str(out / "I18nStrings.h"))
        generate_strings_cpp(
            languages, language_names, string_keys, translations, str(out / "I18nStrings.cpp")
        )

        print()
        print("✓ Code generation complete!")
        print(f"  Languages: {len(languages)}")
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
