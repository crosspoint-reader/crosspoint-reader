#!/usr/bin/env python3
"""
Generate the expat-full HTML renderer test dictionary (grouped by tag category).

Output: test/dictionaries/expat-full/
  expat-full.ifo        — metadata
  expat-full.dict       — definitions (HTML, sametypesequence=h)
  expat-full.idx        — index (headwords, sorted)
  expat-full.idx.oft    — index offset table (stride 32)

Six entries — one per tag category. Each definition contains all tags in its
category so that a single lookup exercises the full category. This minimises
device-testing friction compared to one entry per tag.

Each definition structure:
  <description paragraph(s) in plain text — no formatting tags>
  <p>----------</p>
  <test HTML>
  <p>----------</p>

The plain-text description is intentionally unformatted so that expat bugs
in the test HTML cannot corrupt the description text.

Format references (verified against project code and real files):
  .ifo   : key=value text; version=2.4.2
  .idx   : [word\\0][uint32 offset BE][uint32 size BE], sorted lexicographically
  .oft   : 38-byte header + LE uint32 offsets at stride=32
            header = b"StarDict's Cache, Version: 0.2" (30 bytes, no null)
                   + b"\\xc1\\xd1\\xa4\\x51\\x00\\x00\\x00\\x00" (8 bytes fixed magic)
            entry N = byte offset of word N*32 in .idx
            word 0 is NOT stored; first entry covers ordinal 32
  sametypesequence=h : HTML; size from .idx size field (no null terminator)
"""

import os
import struct

# ---------------------------------------------------------------------------
# Entries: (headword, html_definition)
# Must be sorted lexicographically — generator sorts them.
# Six entries, one per tag category.
# ---------------------------------------------------------------------------

SEP = "<p>----------</p>"

ENTRIES = [
    # ------------------------------------------------------------------
    # 1. abbr title expansion
    #    Three cases: single-word title, multi-word title, element inside abbr.
    # ------------------------------------------------------------------
    (
        "AbbrExpand",
        (
            "<p>Three abbreviations should expand inline with their full title in parentheses.</p>"
            "<p>Case 1: single-word title. Expected: c. (circa)</p>"
            "<p>Case 2: multi-word title containing a space. Expected: AD (anno Domini)</p>"
            "<p>Case 3: italic element inside abbr. Expected: f. (filius) with f. rendered italic.</p>"
            + SEP
            + "<p>Abbreviation one: <abbr title=\"circa\">c.</abbr></p>"
            + "<p>Abbreviation two: <abbr title=\"anno Domini\">AD</abbr></p>"
            + "<p>Abbreviation three: <abbr title=\"filius\"><i>f.</i></abbr></p>"
            + SEP
        ),
    ),

    # ------------------------------------------------------------------
    # 2. Strip entirely — hiero, svg, math, gallery, nowiki, poem, ref, REF
    #    Nothing should appear between the rules (all 8 blocks stripped).
    # ------------------------------------------------------------------
    (
        "BlockStrip",
        (
            "<p>Eight block tags and all their children should be stripped entirely.</p>"
            "<p>Nothing should appear between the two rules below.</p>"
            "<p>Tags tested: hiero (with nested table/tr/td/img), svg (with defs/g/path/rect),"
            " math (with sup), gallery, nowiki, poem, ref (lowercase), REF (uppercase).</p>"
            + SEP
            + "<hiero><table class=\"mw-hiero-table\"><tr><td>"
            + "<img src=\"x.gif\" alt=\"glyph\"/></td></tr></table></hiero>"
            + "<svg width=\"10\" height=\"10\"><defs/><g>"
            + "<path d=\"M 0 0\"/><rect width=\"5\" height=\"5\"/></g></svg>"
            + "<math>x<sup>2</sup> + y<sup>2</sup></math>"
            + "<gallery>Image:x.jpg|hidden caption</gallery>"
            + "<nowiki><b>hidden nowiki content</b></nowiki>"
            + "<poem>hidden poem line</poem>"
            + "<ref name=\"x\">hidden ref content</ref>"
            + "<REF NAME=\"x\">hidden REF content</REF>"
            + SEP
        ),
    ),

    # ------------------------------------------------------------------
    # 3. Block structure — p, div, br, blockquote, ol+li, ul+li, h1-h4
    # ------------------------------------------------------------------
    (
        "BlockStruct",
        (
            "<p>Block structure elements. Expected output in order:</p>"
            "<p>Two separate paragraphs. Two div blocks. Three lines separated by br."
            " An indented blockquote. A numbered list (first, second, third)."
            " A bulleted list (alpha, beta, gamma)."
            " Four bold headings (Heading One through Heading Four).</p>"
            + SEP
            + "<p>First paragraph.</p>"
            + "<p>Second paragraph.</p>"
            + "<div>First div block.</div>"
            + "<div>Second div block.</div>"
            + "<p>Line one.<br/>Line two.<br/>Line three.</p>"
            + "<blockquote>indented passage</blockquote>"
            + "<ol><li>first</li><li>second</li><li>third</li></ol>"
            + "<ul><li>alpha</li><li>beta</li><li>gamma</li></ul>"
            + "<h1>Heading One</h1>"
            + "<h2>Heading Two</h2>"
            + "<h3>Heading Three</h3>"
            + "<h4>Heading Four</h4>"
            + SEP
        ),
    ),

    # ------------------------------------------------------------------
    # 4. Inline formatting — b, strong, i, em, u, s, sub, sup, code, tt,
    #    small, big, var, and two nested combinations.
    # ------------------------------------------------------------------
    (
        "FormatTags",
        (
            "<p>Inline formatting tags. Expected words with their styles:</p>"
            "<p>bold (b), bold (strong), italic (i), italic (em), underline (u),"
            " strikethrough (s), subscript 2 in H2O (sub), superscript 2 in x2 (sup),"
            " code style (code), code style (tt), small size (small), big size (big),"
            " italic var (var).</p>"
            "<p>Nested: bold-italic (b+i). Triple: bold-italic-underline (b+i+u).</p>"
            + SEP
            + "<p><b>bold</b> <strong>bold</strong> <i>italic</i> <em>italic</em>"
            + " <u>underline</u> <s>strike</s>"
            + " H<sub>2</sub>O x<sup>2</sup>"
            + " <code>printf</code> <tt>mono</tt>"
            + " <small>small</small> <big>big</big>"
            + " <var>count</var></p>"
            + "<p>Nested: <b><i>bold-italic</i></b>"
            + " <b><i><u>bold-italic-underline</u></i></b></p>"
            + SEP
        ),
    ),

    # ------------------------------------------------------------------
    # 5. Strip tag, keep text — span, single unknown tag, nested unknown tags
    # ------------------------------------------------------------------
    (
        "StripKeep",
        (
            "<p>Three cases where the tag is stripped but its text content is kept.</p>"
            "<p>Case 1: span tag stripped, text kept. Expected: visible span text</p>"
            "<p>Case 2: single unknown tag stripped, text kept. Expected: visible unknown text</p>"
            "<p>Case 3: nested unknown tags both stripped, innermost text kept."
            " Expected: visible nested text</p>"
            + SEP
            + "<p><span>visible span text</span></p>"
            + "<p><unknowntag>visible unknown text</unknowntag></p>"
            + "<p><outertag><innertag>visible nested text</innertag></outertag></p>"
            + SEP
        ),
    ),

    # ------------------------------------------------------------------
    # 6. Wikitext annotation tags — self-closing <XX:YY/> style.
    #    Text is extracted from the tag name suffix (part after the colon).
    #    Eight annotation types: t:, tr:, lang:, gloss:, pos:, sc:, alt:, id:
    # ------------------------------------------------------------------
    (
        "WikiAnnot",
        (
            "<p>Eight wikitext annotation tags. Each is a self-closing tag of the form"
            " XX:YY where the text to render is the suffix YY (the part after the colon).</p>"
            "<p>Expected inline text in order: four, oikos, la, sharp, noun, Grek, ameba, female</p>"
            + SEP
            + "<p>"
            + "count: <t:four/> "
            + "origin: <tr:oikos/> "
            + "language: <lang:la/> "
            + "meaning: <gloss:sharp/> "
            + "part of speech: <pos:noun/> "
            + "script: <sc:Grek/> "
            + "alternate: <alt:ameba/> "
            + "identifier: <id:female/>"
            + "</p>"
            + SEP
        ),
    ),
]


# ---------------------------------------------------------------------------
# StarDict format helpers (identical to other gen_*_dict.py scripts)
# ---------------------------------------------------------------------------
OFT_HEADER = b"StarDict's Cache, Version: 0.2" + b"\xc1\xd1\xa4\x51\x00\x00\x00\x00"
assert len(OFT_HEADER) == 38
STRIDE = 32


def build_oft(offsets_at_stride):
    """Build .oft file bytes given list of byte offsets at stride boundaries."""
    data = OFT_HEADER
    for off in offsets_at_stride:
        data += struct.pack("<I", off)
    return data


def build_idx_and_dict(entries_sorted):
    """
    Build .idx and .dict binaries from sorted (headword, html_definition) list.
    Returns (idx_bytes, dict_bytes).
    """
    idx = b""
    dict_bytes = b""
    for word, defn in entries_sorted:
        defn_b = defn.encode("utf-8")
        off = len(dict_bytes)
        size = len(defn_b)
        dict_bytes += defn_b
        idx += word.encode("utf-8") + b"\x00"
        idx += struct.pack(">II", off, size)
    return idx, dict_bytes


def oft_offsets_for_idx(idx_bytes, stride):
    """
    Walk .idx binary and collect byte offsets of every stride-th entry.
    Each entry: word\\0 + 4-byte offset BE + 4-byte size BE = word\\0 + 8 bytes.
    Entry 0 is not stored; first recorded offset is after entry stride-1.
    """
    offsets = []
    entry_count = 0
    pos = 0
    while pos < len(idx_bytes):
        null = idx_bytes.index(b"\x00", pos)
        pos = null + 1 + 8
        entry_count += 1
        if entry_count % stride == 0:
            offsets.append(pos)
    return offsets


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    out_dir = os.path.join(os.path.dirname(__file__), "..", "test", "dictionaries", "expat-full")
    os.makedirs(out_dir, exist_ok=True)
    stem = os.path.join(out_dir, "expat-full")

    # Sort lexicographically (StarDict requirement)
    entries_sorted = sorted(ENTRIES, key=lambda e: e[0])

    print(f"Entries: {len(entries_sorted)}")
    for word, _ in entries_sorted:
        print(f"  {word}")

    # Build binaries
    idx_bytes, dict_bytes = build_idx_and_dict(entries_sorted)
    idx_oft_offsets = oft_offsets_for_idx(idx_bytes, STRIDE)

    print(f"idx.oft data entries: {len(idx_oft_offsets)} (0 expected for <32 headwords)")

    # Write files
    with open(stem + ".dict", "wb") as f:
        f.write(dict_bytes)

    with open(stem + ".idx", "wb") as f:
        f.write(idx_bytes)

    with open(stem + ".idx.oft", "wb") as f:
        f.write(build_oft(idx_oft_offsets))

    ifo_lines = [
        "StarDict's dict ifo file",
        "version=2.4.2",
        f"wordcount={len(entries_sorted)}",
        f"idxfilesize={len(idx_bytes)}",
        "bookname=ExpAT Full Test",
        "sametypesequence=h",
    ]
    with open(stem + ".ifo", "w", encoding="utf-8") as f:
        f.write("\n".join(ifo_lines) + "\n")

    print(f"\nFiles written to {out_dir}/")
    for ext in [".ifo", ".dict", ".idx", ".idx.oft"]:
        path = stem + ext
        size = os.path.getsize(path)
        print(f"  expat-full{ext:10s}  {size:6d} bytes")


if __name__ == "__main__":
    main()
