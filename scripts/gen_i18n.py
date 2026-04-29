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

By default the script scans the src/ and lib/ trees for STR_* references and
reports any translation keys that are never used.  Pass --strip-unused to
omit those keys from the generated output entirely.

Usage:
    python gen_i18n.py [translations_dir [output_dir]] [options]

Examples:
    python gen_i18n.py
    python gen_i18n.py lib/I18n/translations lib/I18n/
    python gen_i18n.py --strip-unused
    python gen_i18n.py --strip-unused --src-dirs src lib/EpdFont
"""

import sys
import os
import re
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple


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
                raise ValueError(f"{filepath}:{line_num}: unknown escape '\\{nxt}'")
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
    verbose: bool = False,
) -> Tuple[List[str], List[str], List[str], Dict[str, List[str]], List[Set[str]]]:
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

    duplicate_orders: Dict[str, List[str]] = {}
    order_to_files: Dict[str, List[str]] = {}
    for fname, data in parsed.items():
        order = data.get("_order")
        if not order:
            continue
        order_to_files.setdefault(order, []).append(fname)

    for order, files in order_to_files.items():
        if len(files) > 1:
            duplicate_orders[order] = sorted(files)

    if duplicate_orders:
        duplicate_messages = [
            f"_order {order}: {', '.join(files)}"
            for order, files in sorted(
                duplicate_orders.items(), key=lambda item: int(item[0])
            )
        ]
        raise ValueError(
            "Duplicate _order values found:\n  "
            + "\n  ".join(duplicate_messages)
            + "\nEach _order value must be unique to ensure a deterministic language order."
        )

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
    inherited_sets: List[Set[str]] = [set() for _ in ordered_files]
    translations: Dict[str, List[str]] = {}
    for key in string_keys:
        row: List[str] = []
        for lang_idx, fname in enumerate(ordered_files):
            data = parsed[fname]
            value = data.get(key, "")
            if not value.strip() and fname != english_file:
                value = english_data[key]
                inherited_sets[lang_idx].add(key)
                if verbose:
                    print(
                        f"  INFO: '{key}' missing in {language_codes[lang_idx]}, using English fallback"
                    )
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
            if verbose:
                print(
                    f"  WARNING: {lang_code} has keys not in English: {', '.join(extra)}"
                )

    if verbose:
        print(f"Loaded {len(language_codes)} languages, {len(string_keys)} string keys")
    return language_codes, language_names, string_keys, translations, inherited_sets


# ---------------------------------------------------------------------------
# Unused-string detection
# ---------------------------------------------------------------------------

_GENERATED_FILENAMES: Set[str] = {"I18nKeys.h", "I18nStrings.h", "I18nStrings.cpp"}


def find_used_string_keys(
    src_dirs: List[str],
    skip_filenames: Optional[Set[str]] = None,
) -> Set[str]:
    """
    Scan C/C++ source files under *src_dirs* for STR_* identifiers.

    Files whose basename appears in *skip_filenames* are skipped so that
    the generated I18n files don't count as "usage" of themselves.

    Returns the set of all STR_KEY names that appear at least once.
    """
    if skip_filenames is None:
        skip_filenames = _GENERATED_FILENAMES

    pattern = re.compile(r"\bSTR_[A-Za-z0-9_]+\b")
    used: Set[str] = set()

    for src_dir in src_dirs:
        p = Path(src_dir)
        if not p.is_dir():
            continue
        for f in p.rglob("*"):
            if f.suffix not in {".cpp", ".h", ".c"}:
                continue
            if f.name in skip_filenames:
                continue
            try:
                text = f.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            for m in pattern.finditer(text):
                used.add(m.group(0))

    return used


def report_unused_keys(
    string_keys: List[str],
    used_keys: Set[str],
) -> List[str]:
    """Return a sorted list of keys from *string_keys* absent in *used_keys*."""
    return [k for k in sorted(string_keys) if k not in used_keys]


# ---------------------------------------------------------------------------
# C++ string escaping
# ---------------------------------------------------------------------------


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
            if nxt in 'ntr"\\':
                current.append(ch + nxt)
                i += 2
            elif nxt == "x" and i + 3 < len(s):
                current.append(s[i : i + 4])
                _flush()  # segment break after hex
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
                _flush()  # segment break after hex
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
                if current[idx] == " ":
                    last_space = idx

                # Handle escapes to step correctly
                if current[idx] == "\\":
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
    languages: List[str],
    language_names: List[str],
    string_keys: List[str],
    output_path: str,
    verbose: bool = False,
) -> None:
    """Generate I18nKeys.h."""
    lines: List[str] = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "// clang-format off",
        "",
        "// Forward declarations for flat string data blobs and offset tables",
        "namespace i18n_strings {",
    ]

    for code in languages:
        lines.append(f"extern const char STRINGS_{code}_DATA[];")
        lines.append(f"extern const uint16_t OFFSETS_{code}[];")

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
    lines.append("// Language codes (defined in I18nStrings.cpp)")
    lines.append("extern const char* const LANGUAGE_CODES[];")
    lines.append("")

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

    # LangStrings struct
    lines.append("// Holds a flat string blob and its offset table for one language")
    lines.append("struct LangStrings {")
    lines.append("  const char* data;")
    lines.append("  const uint16_t* offsets;")
    lines.append("};")
    lines.append("")

    # getLanguageStrings helper
    lines.append("// Helper function to get string data for a language")
    lines.append("inline LangStrings getLanguageStrings(Language lang) {")
    lines.append("  switch (lang) {")
    for code in languages:
        lines.append(f"    case Language::{code}:")
        lines.append(
            f"      return {{i18n_strings::STRINGS_{code}_DATA, i18n_strings::OFFSETS_{code}}};"
        )
    first_code = languages[0]
    lines.append("    default:")
    lines.append(
        f"      return {{i18n_strings::STRINGS_{first_code}_DATA, i18n_strings::OFFSETS_{first_code}}};"
    )
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
    lines.append("// Sorted language indices by code (auto-generated by gen_i18n.py)")
    for rank, idx in enumerate(sorted_indices):
        lines.append(f"//   {rank:>2}: {languages[idx]:<4} {language_names[idx]}")
    lines.append(
        "constexpr uint8_t SORTED_LANGUAGE_INDICES[] = {"
        f"{', '.join(str(i) for i in sorted_indices)}"
        "};"
    )
    lines.append("")
    lines.append(
        "static_assert(sizeof(SORTED_LANGUAGE_INDICES) / sizeof(SORTED_LANGUAGE_INDICES[0]) == getLanguageCount(),"
    )
    lines.append('              "SORTED_LANGUAGE_INDICES size mismatch");')

    _write_file(output_path, lines, verbose)


def generate_strings_header(
    languages: List[str],
    language_names: List[str],
    output_path: str,
    verbose: bool = False,
) -> None:
    """Generate I18nStrings.h."""
    lines: List[str] = [
        "#pragma once",
        '#include "I18nKeys.h"',
        "",
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "// clang-format off",
        "",
        "namespace i18n_strings {",
        "",
    ]

    for code in languages:
        lines.append(f"extern const char STRINGS_{code}_DATA[];")
        lines.append(f"extern const uint16_t OFFSETS_{code}[];")

    lines.append("")
    lines.append("}  // namespace i18n_strings")
    _write_file(output_path, lines, verbose)


def generate_strings_cpp(
    languages: List[str],
    language_names: List[str],
    string_keys: List[str],
    translations: Dict[str, List[str]],
    output_path: str,
    verbose: bool = False,
) -> None:
    """Generate I18nStrings.cpp."""
    lines: List[str] = [
        "// THIS FILE IS AUTO-GENERATED BY gen_i18n.py. DO NOT EDIT.",
        "// clang-format off",
        '#include "I18nStrings.h"',
        "",
        "#include <cstddef>",
        "",
    ]

    # LANGUAGE_NAMES array
    lines.append("// Language codes")
    lines.append("const char* const LANGUAGE_CODES[] = {")
    for code in languages:
        _append_string_entry(lines, code)
    lines.append("};")
    lines.append("")

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

    # Per-language flat string blobs and offset tables
    lines.append("namespace i18n_strings {")
    lines.append("")

    for lang_idx, code in enumerate(languages):
        lang_strings = [translations[key][lang_idx] for key in string_keys]

        # Precompute byte offsets (UTF-8 encoded, +1 per string for null terminator)
        offsets: List[int] = []
        current_offset = 0
        for s in lang_strings:
            offsets.append(current_offset)
            current_offset += len(s.encode("utf-8")) + 1
        if current_offset > 65535:
            raise ValueError(
                f"Language {code}: total string data ({current_offset} bytes) "
                "exceeds uint16_t offset range (65535)"
            )

        # Flat string data blob — all strings concatenated with \0 separators.
        lines.append(f"const char STRINGS_{code}_DATA[] =")
        for text in lang_strings:
            _append_string_data_entry(lines, text)
        lines.append(";")
        lines.append("")

        # Offset table — one uint16_t per StrId
        lines.append(f"const uint16_t OFFSETS_{code}[] = {{")
        chunk_size = 12
        for i in range(0, len(offsets), chunk_size):
            chunk = offsets[i : i + chunk_size]
            lines.append("    " + ", ".join(str(o) for o in chunk) + ",")
        lines.append("};")
        lines.append("")

    lines.append("}  // namespace i18n_strings")
    lines.append("")

    # Compile-time size checks
    lines.append("// Compile-time validation of array sizes")
    for code in languages:
        lines.append(
            f"static_assert(sizeof(i18n_strings::OFFSETS_{code}) "
            f"/ sizeof(i18n_strings::OFFSETS_{code}[0]) =="
        )
        lines.append("                  static_cast<size_t>(StrId::_COUNT),")
        lines.append(f'              "OFFSETS_{code} size mismatch");')

    _write_file(output_path, lines, verbose)


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _print_language_table(
    language_codes: List[str],
    language_names: List[str],
    inherited_sets: List[Set[str]],
    string_keys: List[str],
    unused_keys: Set[str],
    data_sizes: List[int],
) -> None:
    """Print a per-language summary table."""
    total = len(string_keys)
    headers = ("Language", "Code", "Own", "Fallback", "Unused", "Data (B)")

    rows = []
    for code, name, inherited, size in zip(
        language_codes, language_names, inherited_sets, data_sizes
    ):
        own = total - len(inherited)
        fallback = len(inherited)
        # strings this language translated but the code never calls
        unused = len(unused_keys - inherited)
        rows.append((name, code, str(own), str(fallback), str(unused), str(size)))

    # EN first, then alphabetically by ISO code
    rows.sort(key=lambda r: (0 if r[1] == "EN" else 1, r[1]))

    col_widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            col_widths[i] = max(col_widths[i], len(cell))

    fmt = "  ".join(f"{{:<{w}}}" for w in col_widths)
    sep = "  ".join("-" * w for w in col_widths)

    def _safe_print(line: str) -> None:
        print(
            line.encode(sys.stdout.encoding or "utf-8", errors="replace").decode(
                sys.stdout.encoding or "utf-8", errors="replace"
            )
        )

    _safe_print(fmt.format(*headers))
    _safe_print(sep)
    for row in rows:
        _safe_print(fmt.format(*row))
    used = total - len(unused_keys)
    total_size = sum(data_sizes)
    n_lang = len(rows)
    n_keys = len(string_keys)
    # Current layout: uint16_t offset table (2 B per string per language)
    offset_table_size = n_lang * n_keys * 2
    current_total = total_size + offset_table_size
    # Previous layout: const char* pointer array (4 B per string per language)
    old_pointer_table_size = n_lang * n_keys * 4
    old_total = total_size + old_pointer_table_size
    saved = old_total - current_total
    print(
        f"\n  Total: {total}  |  Used in code: {used}  |  Never used: {len(unused_keys)}"
    )
    print(
        f"  Flash (now):    {total_size:>7,} B strings  +  {offset_table_size:>6,} B offset tables (uint16_t)"
        f"  =  {current_total:>7,} B"
    )
    print(
        f"  Flash (before): {total_size:>7,} B strings  +  {old_pointer_table_size:>6,} B pointer tables (ptr32)"
        f"  =  {old_total:>7,} B"
    )
    print(f"  Saved by offset tables: {saved:,} B")


def _append_string_data_entry(lines: List[str], text: str) -> None:
    """
    Escape *text*, append a \\0 null separator, and format as indented C++
    string literal lines for inclusion in a flat char data array blob.
    """
    segments = escape_cpp_string(text)
    # Append the null entry separator to the last segment
    if segments and segments[-1] != "":
        segments[-1] += "\\0"
    elif segments:
        segments[-1] = "\\0"
    else:
        segments = ["\\0"]
    lines.extend(format_cpp_string_literal(segments))


def _append_string_entry(lines: List[str], text: str, comment: str = "") -> None:
    """Escape *text*, format as indented C++ lines, append comma (and optional comment)."""
    segments = escape_cpp_string(text)
    formatted = format_cpp_string_literal(segments)
    suffix = f",  // {comment}" if comment else ","
    formatted[-1] += suffix
    lines.extend(formatted)


def _write_file(path: str, lines: List[str], verbose: bool = False) -> None:
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
        f.write("\n")
    if verbose:
        print(f"Generated: {path}")


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main(
    translations_dir: Optional[str] = None,
    output_dir: Optional[str] = None,
    src_dirs: Optional[List[str]] = None,
    strip_unused: bool = False,
    verbose: bool = False,
) -> None:
    # Default paths (relative to project root)
    default_translations_dir = "lib/I18n/translations"
    default_output_dir = "lib/I18n/"
    default_src_dirs = ["src", "lib"]

    if translations_dir is None or output_dir is None:
        if len(sys.argv) == 3:
            translations_dir = sys.argv[1]
            output_dir = sys.argv[2]
        else:
            # Default for no arguments or weird arguments (e.g. SCons)
            translations_dir = default_translations_dir
            output_dir = default_output_dir

    if src_dirs is None:
        src_dirs = default_src_dirs

    if not os.path.isdir(translations_dir):
        print(f"Error: Translations directory not found: {translations_dir}")
        sys.exit(1)

    if not os.path.isdir(output_dir):
        print(f"Error: Output directory not found: {output_dir}")
        sys.exit(1)

    if verbose:
        print(f"Reading translations from: {translations_dir}")
        print(f"Output directory: {output_dir}")
        print()

    try:
        languages, language_names, string_keys, translations, inherited_sets = (
            load_translations(translations_dir, verbose)
        )

        # --- Unused-string detection ---
        scan_dirs = [d for d in src_dirs if os.path.isdir(d)]
        if scan_dirs:
            used_keys = find_used_string_keys(scan_dirs)
            unused_set = set(report_unused_keys(string_keys, used_keys))
        else:
            used_keys = set(string_keys)
            unused_set = set()

        # --- Missing-string detection (used in code but absent from English) ---
        missing_keys = sorted(used_keys - set(string_keys))
        if missing_keys:
            print(
                f"\n  CRITICAL: {len(missing_keys)} string(s) used in source but missing from english.yaml:"
            )
            for key in missing_keys:
                print(f"    - {key}")
            print()
            sys.exit(1)

        # Compute per-language data blob sizes:
        # sum of UTF-8 byte length + 1 (null terminator) per string
        data_sizes = [
            sum(len(translations[k][i].encode("utf-8")) + 1 for k in string_keys)
            for i in range(len(languages))
        ]

        _print_language_table(
            languages,
            language_names,
            inherited_sets,
            string_keys,
            unused_set,
            data_sizes,
        )
        print()

        if verbose and unused_set:
            print(f"  Unused keys ({len(unused_set)}):")
            for key in sorted(unused_set):
                print(f"    - {key}")
            print()

        if unused_set and strip_unused:
            string_keys = [k for k in string_keys if k not in unused_set]
            translations = {
                k: v for k, v in translations.items() if k not in unused_set
            }
            inherited_sets = [s - unused_set for s in inherited_sets]
            print(f"  Stripping {len(unused_set)} unused string(s) from output.")

        out = Path(output_dir)
        generate_keys_header(
            languages, language_names, string_keys, str(out / "I18nKeys.h"), verbose
        )
        generate_strings_header(
            languages, language_names, str(out / "I18nStrings.h"), verbose
        )
        generate_strings_cpp(
            languages,
            language_names,
            string_keys,
            translations,
            str(out / "I18nStrings.cpp"),
            verbose,
        )

        print()
        print("Code generation complete!")
        print(f"  Languages: {len(languages)}")
        print(f"  String keys: {len(string_keys)}")
        if unused_set and not strip_unused:
            print(
                f"  Unused keys: {len(unused_set)} (pass --strip-unused to remove them)"
            )

    except Exception as e:
        print(f"\nError: {e}")
        sys.exit(1)


if __name__ == "__main__":
    import argparse

    parser = argparse.ArgumentParser(
        description="Generate I18n C++ files from per-language YAML translations."
    )
    parser.add_argument(
        "translations_dir",
        nargs="?",
        default=None,
        help="Path to the translations directory (default: lib/I18n/translations)",
    )
    parser.add_argument(
        "output_dir",
        nargs="?",
        default=None,
        help="Path to the output directory (default: lib/I18n/)",
    )
    parser.add_argument(
        "--src-dirs",
        nargs="+",
        metavar="DIR",
        default=None,
        help="Source directories to scan for STR_* usage (default: src lib)",
    )
    parser.add_argument(
        "--strip-unused",
        action="store_true",
        help="Remove unused STR_* keys from the generated output",
    )
    parser.add_argument(
        "--verbose",
        "-v",
        action="store_true",
        help="Print per-key INFO/WARNING messages and file generation details",
    )
    args = parser.parse_args()
    main(
        args.translations_dir,
        args.output_dir,
        args.src_dirs,
        args.strip_unused,
        args.verbose,
    )
else:
    try:
        Import("env")
        main(strip_unused=True)
    except NameError:
        pass
