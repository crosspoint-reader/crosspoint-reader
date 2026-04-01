#!/usr/bin/env python3
"""Convert dictionary.txt to StarDict format with inflected forms.

Usage: python3 scripts/convert_dictionary.py dictionary.txt english

Input format (dictionary.txt):
  Word  definition text here
  (blank lines between entries, two-space separator)

Output files (StarDict format):
  <name>.ifo  — metadata (bookname, wordcount, idxfilesize, sametypesequence)
  <name>.idx  — binary index: [word\\0][uint32 offset BE][uint32 size BE], sorted
  <name>.dict — raw definition data at byte offsets

Also generates <name>.idx.cp — a secondary index for fast binary search on the
ESP32-C3 device (avoids slow on-device index generation at first boot).

Inflected forms (e.g., "walks", "walked", "walking") are generated from base
words and point to "See: <base word>" definitions. Only forms not already in
the dictionary are added.
"""
import struct
import sys
import re


def generate_inflections(word, definition):
    """Generate common English inflected forms from a base word.

    Returns a list of (inflected_form, base_word) tuples.
    Only generates forms for simple single words (no hyphens, spaces, etc.).
    Uses the definition's part-of-speech markers to decide which forms to generate.
    """
    # Skip multi-word entries, hyphenated words, very short words, or non-alpha
    if ' ' in word or '-' in word or len(word) < 3 or not word[0].isalpha():
        return []
    # Skip words ending in common suffixes that are already inflected
    if word.endswith(('ing', 'tion', 'sion', 'ness', 'ment', 'ous', 'ible', 'able')):
        return []

    forms = []
    defn_lower = definition.lower()

    # Detect likely parts of speech from definition markers (scan full definition)
    is_verb = any(m in defn_lower for m in ('—v.', 'v. ', 'v.i.', 'v.t.', 'past ', 'past part.'))
    is_noun = any(m in defn_lower for m in ('—n.', 'n. ', 'n.pl.'))
    is_adj = any(m in defn_lower for m in ('—adj.', 'adj. ', '—attrib.', 'attrib. '))
    is_adv = any(m in defn_lower for m in ('—adv.', 'adv. '))
    is_prefix = any(m in defn_lower for m in ('prefix', 'comb. form', 'suffix'))

    # Skip prefixes, suffixes, combining forms
    if is_prefix:
        return []

    # If no POS detected, only generate -s (safest universal inflection)
    if not (is_verb or is_noun or is_adj or is_adv):
        if word.endswith(('s', 'x', 'z', 'sh', 'ch')):
            forms.append((word + 'es', word))
        elif word.endswith('y') and word[-2] not in 'aeiou':
            forms.append((word[:-1] + 'ies', word))
        else:
            forms.append((word + 's', word))
        return forms

    # --- Plurals / third-person -s (nouns and verbs) ---
    if is_noun or is_verb:
        if word.endswith(('s', 'x', 'z', 'sh', 'ch')):
            forms.append((word + 'es', word))
        elif word.endswith('y') and word[-2] not in 'aeiou':
            forms.append((word[:-1] + 'ies', word))
        else:
            forms.append((word + 's', word))

    # --- Verb-specific: -ed, -ing ---
    if is_verb:
        # -ed (past tense)
        if word.endswith('e'):
            forms.append((word + 'd', word))
        elif word.endswith('y') and word[-2] not in 'aeiou':
            forms.append((word[:-1] + 'ied', word))
        elif (len(word) >= 3 and word[-1] not in 'aeiouwxy'
              and word[-2] in 'aeiou' and word[-3] not in 'aeiou'):
            forms.append((word + word[-1] + 'ed', word))
        else:
            forms.append((word + 'ed', word))

        # -ing (present participle)
        if word.endswith('ie'):
            forms.append((word[:-2] + 'ying', word))
        elif word.endswith('e') and not word.endswith('ee'):
            forms.append((word[:-1] + 'ing', word))
        elif (len(word) >= 3 and word[-1] not in 'aeiouwxy'
              and word[-2] in 'aeiou' and word[-3] not in 'aeiou'):
            forms.append((word + word[-1] + 'ing', word))
        else:
            forms.append((word + 'ing', word))

    # --- Adjective-specific: -er, -est, -ly, -ness ---
    if is_adj:
        if word.endswith('e'):
            forms.append((word + 'r', word))
            forms.append((word + 'st', word))
        elif word.endswith('y') and word[-2] not in 'aeiou':
            forms.append((word[:-1] + 'ier', word))
            forms.append((word[:-1] + 'iest', word))
            forms.append((word[:-1] + 'ily', word))
            forms.append((word[:-1] + 'iness', word))
        else:
            forms.append((word + 'er', word))
            forms.append((word + 'est', word))
            forms.append((word + 'ly', word))
            forms.append((word + 'ness', word))

    return forms


DICT_WORD_MAX = 32  # Must match DictTypes.h

# Secondary index (.idx.cp) constants — must match DictTypes.h
SD_INDEX_MAGIC = 0x43504958  # "CPIX"
SD_INDEX_VERSION = 1
SD_INDEX_HEADER_SIZE = 16
SD_INDEX_ENTRY_SIZE = 44


def write_stardict(entries, base_path, bookname):
    """Write StarDict .ifo/.idx/.dict files and secondary .idx.cp index.

    Args:
        entries: list of (word, definition) tuples, already sorted
        base_path: output path without extension (e.g., "english")
        bookname: display name for .ifo bookname field
    """
    dict_path = base_path + '.dict'
    idx_path = base_path + '.idx'
    ifo_path = base_path + '.ifo'
    cp_idx_path = base_path + '.idx.cp'

    # --- Write .dict: concatenated raw definitions ---
    # Convert escaped \\n to real newlines for StarDict format
    offsets = []  # (word, dict_offset, dict_size) for each entry
    with open(dict_path, 'wb') as f:
        for word, defn in entries:
            # Convert literal \\n sequences to real newlines
            raw_defn = defn.replace('\\n', '\n')
            defn_bytes = raw_defn.encode('utf-8')
            offset = f.tell()
            f.write(defn_bytes)
            offsets.append((word, offset, len(defn_bytes)))

    dict_size = sum(size for _, _, size in offsets)
    print(f'Wrote {dict_path} ({dict_size} bytes)')

    # --- Write .idx: [word\0][uint32 offset BE][uint32 size BE] per entry ---
    with open(idx_path, 'wb') as f:
        for word, offset, size in offsets:
            word_bytes = word.encode('utf-8')
            f.write(word_bytes + b'\x00')
            f.write(struct.pack('>II', offset, size))
        idx_size = f.tell()
    print(f'Wrote {idx_path} ({idx_size} bytes, {len(offsets)} entries)')

    # --- Write .ifo: StarDict metadata ---
    with open(ifo_path, 'w', encoding='utf-8') as f:
        f.write('StarDict\'s dict ifo file\n')
        f.write('version=2.4.2\n')
        f.write(f'wordcount={len(offsets)}\n')
        f.write(f'idxfilesize={idx_size}\n')
        f.write(f'bookname={bookname}\n')
        f.write('sametypesequence=m\n')
    print(f'Wrote {ifo_path} (bookname="{bookname}", wordcount={len(offsets)})')

    # --- Write .idx.cp: secondary index for fast binary search on device ---
    # This saves the device from having to generate it on first boot.
    idx_word_offset = 0  # Track byte offset of each word within .idx
    with open(cp_idx_path, 'wb') as f:
        # Header: magic, version, idxFileSize, entryCount (all little-endian)
        f.write(struct.pack('<IIII', SD_INDEX_MAGIC, SD_INDEX_VERSION, idx_size, len(offsets)))
        for word, offset, size in offsets:
            # Truncate word to DICT_WORD_MAX-1 chars, null-padded to DICT_WORD_MAX bytes
            word_bytes = word.encode('utf-8')
            current_idx_word_offset = idx_word_offset
            idx_word_offset += len(word_bytes) + 1 + 8  # word\0 + offset(4) + size(4)

            truncated = word_bytes[:DICT_WORD_MAX - 1]
            # Ensure truncation doesn't split a multi-byte UTF-8 character
            truncated = truncated.decode('utf-8', 'ignore').encode('utf-8')
            padded = truncated + b'\x00' * (DICT_WORD_MAX - len(truncated))

            f.write(padded)  # 32 bytes
            f.write(struct.pack('<I', offset))  # dictOffset (LE)
            f.write(struct.pack('<I', size))  # dictSize (LE)
            f.write(struct.pack('<I', current_idx_word_offset))  # idxWordOffset (LE)

    cp_size = SD_INDEX_HEADER_SIZE + len(offsets) * SD_INDEX_ENTRY_SIZE
    print(f'Wrote {cp_idx_path} ({cp_size} bytes, {len(offsets)} entries)')


def is_headword_line(line):
    """Check if a line is a Webster's ALL-CAPS headword (e.g., 'ABASE', 'AARD-VARK')."""
    stripped = line.strip()
    if not stripped or len(stripped) < 1:
        return False
    # Headwords are uppercase letters, hyphens, spaces, apostrophes, semicolons
    # Must contain at least one letter and no lowercase letters
    has_letter = False
    for ch in stripped:
        if ch.isupper():
            has_letter = True
        elif ch in " -';,&1234567890":
            continue
        else:
            return False
    return has_letter


def parse_webster(input_path):
    """Parse Webster's Unabridged Dictionary (Project Gutenberg format).

    Format: headword in ALL CAPS on its own line, followed by pronunciation,
    etymology, and definition lines. Entries separated by blank lines.
    """
    entries = []
    with open(input_path, 'r', encoding='utf-8') as f:
        lines = f.readlines()

    # Skip Gutenberg header — find "*** START OF" line
    start_idx = 0
    for i, line in enumerate(lines):
        if '*** START OF' in line:
            start_idx = i + 1
            break

    current_word = None
    current_def_lines = []

    def flush_entry():
        nonlocal current_word, current_def_lines
        if current_word and current_def_lines:
            # Join definition lines, inserting \n before numbered definitions
            # so they display as separate paragraphs on the device.
            parts = []
            for dl in current_def_lines:
                dl = re.sub(r'\s+', ' ', dl).strip()
                if not dl:
                    continue
                # Insert double newline before numbered definitions (e.g., "1.", "2.")
                if re.match(r'^\d+\.', dl) and parts:
                    parts.append('\\n\\n' + dl)
                else:
                    parts.append(dl)
            definition = ' '.join(parts)
            definition = definition.strip()
            if definition:
                # Handle multiple headwords separated by semicolons (e.g., "AARONIC; AARONICAL")
                for hw in current_word.split(';'):
                    hw = hw.strip().lower()
                    if hw:
                        entries.append((hw, definition))
        current_word = None
        current_def_lines = []

    i = start_idx
    while i < len(lines):
        line = lines[i].rstrip('\n\r')
        i += 1

        if not line.strip():
            continue

        if is_headword_line(line):
            flush_entry()
            current_word = line.strip()
            # Skip pronunciation/grammar line(s) — read until first blank or Defn:/numbered def
            while i < len(lines):
                next_line = lines[i].rstrip('\n\r')
                if not next_line.strip():
                    i += 1
                    break
                # If this looks like definition content, don't skip it
                if next_line.strip().startswith(('Defn:', '1.', '2.')):
                    break
                i += 1
            continue

        if current_word is None:
            continue

        stripped = line.strip()
        # Strip "Defn: " prefix
        if stripped.startswith('Defn: '):
            stripped = stripped[6:]
        elif stripped.startswith('Defn:'):
            stripped = stripped[5:]
        # Skip lines that are just etym/note markers
        if stripped.startswith(('Etym:', 'Note:', 'Syn.', '-- ')):
            continue
        # Skip subject markers like "(Mus.)", "(Bot.)", "(Arch.)" on their own
        if re.match(r'^\(\w+\.?\)$', stripped):
            continue

        current_def_lines.append(stripped)

    flush_entry()
    return entries


def parse_simple(input_path):
    """Parse simple two-space-separated format: 'Word  definition text'."""
    entries = []
    with open(input_path, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.rstrip('\n\r')
            if not line.strip():
                continue
            match = re.match(r'^(\S+(?:\s\S+)*?)\s{2,}(.+)$', line)
            if match:
                word = match.group(1).strip().lower()
                definition = match.group(2).strip()
                if word and definition:
                    entries.append((word, definition))
    return entries


def detect_format(input_path):
    """Auto-detect dictionary format by scanning first ~100 non-blank lines."""
    with open(input_path, 'r', encoding='utf-8') as f:
        headword_count = 0
        twospace_count = 0
        for i, line in enumerate(f):
            if i > 200:
                break
            line = line.rstrip('\n\r')
            if not line.strip():
                continue
            if is_headword_line(line):
                headword_count += 1
            if re.match(r'^(\S+(?:\s\S+)*?)\s{2,}(.+)$', line):
                twospace_count += 1
    if headword_count > twospace_count:
        return 'webster'
    return 'simple'


def renumber_definitions(defn):
    """Renumber all N. entries sequentially and ensure 1. is separated from pre-text.

    Only renumbers entries that follow \\n\\n (the double-newline separator inserted
    by flush_entry and the merge step) or appear at the very start of the definition.
    This avoids false positives on numbers in flowing text (e.g., "Acts xvi. 29.").

    Also ensures the first numbered entry has a double-newline before it if preceded
    by other text (e.g., "See: account. 1. ..." becomes "See: account.\\n\\n1. ...").
    """
    SEP = '\\n\\n'  # literal 4-char separator in the dict string

    # Step 1: Ensure first numbered entry is separated from pre-text.
    # If the definition doesn't start with a number, find the first N.\s pattern
    # and insert a separator before it (using string ops, not re.sub replacement,
    # to avoid re.sub interpreting \n as a newline character).
    m = re.match(r'^(.+?\S)\s+(1\.\s)', defn)
    if m and SEP not in defn[:m.start(2)]:
        defn = defn[:m.end(1)] + SEP + defn[m.start(2):]

    # Step 2: Renumber all N. entries sequentially.
    # Match \d+. only at the very start of the string or after the literal
    # backslash-n-backslash-n separator.
    counter = [0]

    def repl(m):
        counter[0] += 1
        return m.group(1) + f'{counter[0]}.'

    defn = re.sub(r'(^|\\n\\n)(\d+)\.', repl, defn)

    return defn


def convert(input_path, base_name):
    fmt = detect_format(input_path)
    print(f'Detected format: {fmt}')

    if fmt == 'webster':
        entries = parse_webster(input_path)
    else:
        entries = parse_simple(input_path)

    # Build set of existing words
    existing = {w for w, _ in entries}

    # Generate inflected forms
    inflection_count = 0
    for word, definition in list(entries):
        for inflected, base in generate_inflections(word, definition):
            if inflected not in existing:
                entries.append((inflected, f'See: {base}. {definition}'))
                existing.add(inflected)
                inflection_count += 1

    # Sort case-insensitively
    entries.sort(key=lambda e: e[0].lower())

    # Merge duplicates: combine definitions for the same word with double-newline separator
    seen = {}
    unique = []
    for word, defn in entries:
        if word not in seen:
            seen[word] = len(unique)
            unique.append((word, defn))
        else:
            idx = seen[word]
            existing_word, existing_defn = unique[idx]
            unique[idx] = (existing_word, existing_defn + '\\n\\n' + defn)

    # Post-process: renumber definitions sequentially and ensure 1. is separated from pre-text
    for i, (word, defn) in enumerate(unique):
        unique[i] = (word, renumber_definitions(defn))

    # Title-case the base name for the bookname (e.g., "english" → "English")
    bookname = base_name.replace('_', ' ').title()

    base_count = len(existing) - inflection_count
    print(f'Converted {base_count} base entries + {inflection_count} inflections = {len(unique)} total')

    write_stardict(unique, base_name, bookname)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        print(f'Usage: {sys.argv[0]} <input.txt> <base_name>')
        print(f'  Example: {sys.argv[0]} WebstersEnglishDictionary.txt english')
        print(f'  Produces: english.ifo, english.idx, english.dict, english.idx.cp')
        sys.exit(1)
    convert(sys.argv[1], sys.argv[2])
