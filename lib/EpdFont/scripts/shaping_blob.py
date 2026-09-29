"""OpenType shaping data for complex scripts in .cpfont files.

The firmware shapes complex-script runs (the Indic scripts in SCRIPTS) with
lib/OtShaper at runtime. The shaper needs the font's layout tables, and the renderer needs a
bitmap for every glyph those tables can produce — conjuncts, reph, below-base
forms — most of which have no Unicode codepoint at all.

This module builds both from one fontTools subset of the source font:

  * the subset, WITH outlines, is what FreeType rasterizes by glyph ID; each
    glyph is stored in the .cpfont at codepoint GLYPH_TOKEN_BASE + glyph ID;
  * the same subset with outlines, hinting and naming stripped is the
    "layout blob": a minimal OpenType font (head, hhea, maxp, hmtx, cmap,
    GDEF, GSUB, GPOS) that the shaper reads on the device, plus CPac, the
    lookup filters the shaper would otherwise compute on the device.

Both share one glyph order, so the glyph IDs the shaper emits index the
bitmaps directly.
"""

import io
import struct
from typing import NamedTuple

# Mirrors lib/EpdFont/ShapingTokens.h.
GLYPH_TOKEN_BASE = 0xF0000
GLYPH_TOKEN_MAX_GID = 0xDFFF

SECTION_MAGIC = b"CPSH"
SECTION_VERSION = 1
SECTION_HEADER_FORMAT = "<4sHHIII"  # magic, version, reserved, ppem26_6, blobLength, blobHash
SECTION_HEADER_SIZE = struct.calcsize(SECTION_HEADER_FORMAT)

# Mirrors the flash slot size in lib/EpdFont/FlashBlobCache.h: a larger
# layout font stays in RAM on boards without PSRAM, where it rarely fits.
FLASH_SLOT_BYTES = 128 * 1024


class Script(NamedTuple):
    first: int  # first codepoint of the 128-codepoint Unicode block
    ot_tags: tuple  # OpenType script tags: Indic v1 and v2 (Sinhala has one)
    probe: int  # letter KA: a font that maps it draws the script

    def unicodes(self):
        return range(self.first, self.first + 0x80)


# Mirrors indic::SCRIPTS in lib/Utf8/IndicScripts.h.
SCRIPTS = {
    "devanagari": Script(0x0900, ("deva", "dev2"), 0x0915),
    "bengali":    Script(0x0980, ("beng", "bng2"), 0x0995),
    "gurmukhi":   Script(0x0A00, ("guru", "gur2"), 0x0A15),
    "gujarati":   Script(0x0A80, ("gujr", "gjr2"), 0x0A95),
    "oriya":      Script(0x0B00, ("orya", "ory2"), 0x0B15),
    "tamil":      Script(0x0B80, ("taml", "tml2"), 0x0B95),
    "telugu":     Script(0x0C00, ("telu", "tel2"), 0x0C15),
    "kannada":    Script(0x0C80, ("knda", "knd2"), 0x0C95),
    "malayalam":  Script(0x0D00, ("mlym", "mlm2"), 0x0D15),
    "sinhala":    Script(0x0D80, ("sinh",), 0x0D9A),
}

# Codepoints every Indic script shares (indic::isShared): dandas, ZWNJ/ZWJ
# (which steer conjunct formation) and the dotted circle the shaper inserts for
# broken clusters. The Vedic stress signs U+0951-U+0954 are shared too, but
# only Devanagari fonts need them and they come with its block; elsewhere
# they would pull in ~6 KB of mark positioning for rare text.
SHARED_INTERVALS = [(0x0964, 0x0965), (0x200C, 0x200D), (0x25CC, 0x25CC)]

_LAYOUT_TABLES = {"head", "hhea", "maxp", "hmtx", "cmap", "GDEF", "GSUB", "GPOS"}


def scripts_in_intervals(intervals):
    """Names of the SCRIPTS whose letter KA the intervals include, in SCRIPTS order."""
    return [name for name, script in SCRIPTS.items()
            if any(start <= script.probe <= end for start, end in intervals)]


def font_supports_script(fontfile, script):
    """True when the font maps the script's letter KA and carries a GSUB table."""
    from fontTools.ttLib import TTFont

    font = TTFont(fontfile, fontNumber=0, lazy=True)
    if "GSUB" not in font:
        return False
    return SCRIPTS[script].probe in (font.getBestCmap() or {})


def _unicodes(scripts):
    """Characters that belong to a shaped run of these scripts, plus the space."""
    unicodes = {0x0020}
    for name in scripts:
        unicodes.update(SCRIPTS[name].unicodes())
    for start, end in SHARED_INTERVALS:
        unicodes.update(range(start, end + 1))
    return sorted(unicodes)


def _subset(fontfile, scripts):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    # Keep the source's head.modified: a save-time stamp would give every
    # size its own blob hash, and the firmware shares flash slots and faces
    # between sizes by that hash.
    font = TTFont(fontfile, fontNumber=0, recalcTimestamp=False)
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.layout_scripts = [tag for name in scripts for tag in SCRIPTS[name].ot_tags] + ["DFLT"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.glyph_names = True
    opts.hinting = True
    opts.legacy_kern = False
    opts.drop_tables += ["morx", "mort", "kerx", "feat", "trak", "ankr", "meta", "DSIG"]
    subsetter = subset.Subsetter(opts)
    subsetter.populate(unicodes=_unicodes(scripts))
    subsetter.subset(font)
    return font


def _strip_to_layout(font):
    """Reduce a subset font to the tables the shaper reads."""
    from fontTools.ttLib import newTable

    for tag in list(font.keys()):
        if tag != "GlyphOrder" and tag not in _LAYOUT_TABLES:
            del font[tag]
    # Outlines are gone, so maxp becomes the CFF-style 0.5 table (glyph count
    # only), which is all the shaper needs without glyf/loca.
    num_glyphs = font["maxp"].numGlyphs
    maxp = newTable("maxp")
    maxp.tableVersion = 0x00005000
    maxp.numGlyphs = num_glyphs
    font["maxp"] = maxp
    buf = io.BytesIO()
    font.save(buf, reorderTables=True)
    return buf.getvalue()


# --- Lookup filters (the CPac table) ------------------------------------------
#
# For every GSUB and GPOS lookup, and every subtable of it, a digest of the
# glyphs it can start at (HarfBuzz's three-mask set digest). The firmware
# skips lookups and subtables whose digest rules the current glyphs out;
# fonts without CPac have it computed on the device, costing heap. These
# functions read the compiled table bytes exactly as lib/OtShaper does
# (OtLayout.cpp subtableCoverage/collectCoverage, OtFace.cpp initFilters).

_DIGEST_SHIFTS = (4, 0, 6)
_ALL = (1 << 64) - 1


class _View:
    """Bounds-checked big-endian reads, as ot::Table."""

    def __init__(self, data=b"", start=0, length=None):
        self.data, self.start = data, start
        self.length = (len(data) - start) if length is None else length

    def empty(self):
        return self.length == 0

    def u16(self, o):
        if o < 0 or o + 2 > self.length:
            return 0
        return int.from_bytes(self.data[self.start + o:self.start + o + 2], "big")

    def u32(self, o):
        if o < 0 or o + 4 > self.length:
            return 0
        return int.from_bytes(self.data[self.start + o:self.start + o + 4], "big")

    def at(self, o):
        if o == 0 or o >= self.length:
            return _View()
        return _View(self.data, self.start + o, self.length - o)

    def offset16(self, field):
        return self.at(self.u16(field))

    def offset32(self, field):
        return self.at(self.u32(field))


class _Digest:
    def __init__(self):
        self.masks = [0, 0, 0]

    def add(self, g):
        for i, shift in enumerate(_DIGEST_SHIFTS):
            self.masks[i] |= 1 << ((g >> shift) & 63)

    def add_range(self, a, b):
        if all(m == _ALL for m in self.masks):
            return
        for i, shift in enumerate(_DIGEST_SHIFTS):
            if (b >> shift) - (a >> shift) >= 63:
                self.masks[i] = _ALL
            else:
                ma = 1 << ((a >> shift) & 63)
                mb = 1 << ((b >> shift) & 63)
                self.masks[i] |= (mb + (mb - ma) - (1 if mb < ma else 0)) & _ALL

    def pack(self):
        return b"".join(m.to_bytes(8, "little") for m in self.masks)


def _subtable_coverage(table, lookup_type, st):
    extension = 7 if table == 0 else 9
    if lookup_type == extension:
        if st.u16(0) != 1:
            return _View()
        lookup_type = st.u16(2)
        st = st.offset32(4)
        if lookup_type == extension:
            return _View()
    is_context = (table == 0 and lookup_type == 5) or (table == 1 and lookup_type == 7)
    is_chain = (table == 0 and lookup_type == 6) or (table == 1 and lookup_type == 8)
    if is_context:
        return st.offset16(6) if st.u16(0) == 3 else st.offset16(2)
    if is_chain:
        if st.u16(0) == 3:
            return st.offset16(4 + 2 * st.u16(2) + 2)
        return st.offset16(2)
    if st.empty():
        return _View()
    return st.offset16(2)


def _collect_coverage(coverage, digest):
    fmt = coverage.u16(0)
    if fmt == 1:
        for i in range(coverage.u16(2)):
            digest.add(coverage.u16(4 + 2 * i))
    elif fmt == 2:
        for i in range(coverage.u16(2)):
            digest.add_range(coverage.u16(4 + 6 * i), coverage.u16(6 + 6 * i))


def _sfnt_table(sfnt, tag):
    count = int.from_bytes(sfnt[4:6], "big")
    for i in range(count):
        record = sfnt[12 + 16 * i:28 + 16 * i]
        if record[:4] == tag:
            offset, length = struct.unpack(">II", record[8:16])
            return sfnt[offset:offset + length]
    return b""


def lookup_filters(layout_bytes):
    """The CPac table for a layout font (see docs/file-formats.md)."""
    header_counts = []
    body = b""
    for table, tag in enumerate((b"GSUB", b"GPOS")):
        layout = _View(_sfnt_table(layout_bytes, tag))
        if layout.u16(0) != 1:
            layout = _View()
        lookup_list = layout.offset16(8)
        count = lookup_list.u16(0)
        lookup_digests, starts, subtable_digests = b"", b"", b""
        subtables = 0
        for i in range(count):
            lookup = lookup_list.offset16(2 + 2 * i)
            lookup_type = lookup.u16(0)
            union = _Digest()
            starts += subtables.to_bytes(4, "big")
            for k in range(lookup.u16(4)):
                d = _Digest()
                _collect_coverage(_subtable_coverage(table, lookup_type, lookup.offset16(6 + 2 * k)), d)
                _collect_coverage(_subtable_coverage(table, lookup_type, lookup.offset16(6 + 2 * k)), union)
                subtable_digests += d.pack()
                subtables += 1
            lookup_digests += union.pack()
        header_counts.append((count, subtables))
        body += lookup_digests + starts + subtable_digests
    (gsub_lookups, gsub_subtables), (gpos_lookups, gpos_subtables) = header_counts
    return struct.pack(">HHHHII", 1, 0, gsub_lookups, gpos_lookups, gsub_subtables, gpos_subtables) + body


def _add_lookup_filters(layout_bytes):
    """layout_bytes with a CPac table added; every other table byte for byte."""
    from fontTools.ttLib import TTFont, newTable

    cpac = newTable("CPac")
    cpac.data = lookup_filters(layout_bytes)
    font = TTFont(io.BytesIO(layout_bytes), lazy=True, recalcTimestamp=False)
    font["CPac"] = cpac
    buf = io.BytesIO()
    font.save(buf, reorderTables=True)
    out = buf.getvalue()
    for tag in (b"GSUB", b"GPOS", b"GDEF", b"cmap", b"hmtx"):
        if _sfnt_table(out, tag) != _sfnt_table(layout_bytes, tag):
            raise AssertionError(f"{tag} changed while adding lookup filters")
    return out


def build(fontfile, scripts):
    """Returns (render_font_bytes, layout_blob_bytes, glyph_count) for shaping
    the named SCRIPTS with this font.

    render_font_bytes is the outline subset FreeType rasterizes by glyph ID.
    """
    font = _subset(fontfile, scripts)
    glyph_count = len(font.getGlyphOrder())
    if glyph_count - 1 > GLYPH_TOKEN_MAX_GID:
        raise ValueError(f"{glyph_count} glyphs exceed the glyph token range")
    buf = io.BytesIO()
    font.save(buf)
    render_bytes = buf.getvalue()

    from fontTools.ttLib import TTFont

    layout = TTFont(io.BytesIO(render_bytes), recalcTimestamp=False)
    layout_bytes = _strip_to_layout(layout)
    with_filters = _add_lookup_filters(layout_bytes)
    # The filters only speed shaping up; a layout font that fits the flash
    # slot without them but not with them is worth more in flash.
    if len(with_filters) <= FLASH_SLOT_BYTES or len(layout_bytes) > FLASH_SLOT_BYTES:
        layout_bytes = with_filters
    return render_bytes, layout_bytes, glyph_count


def fnv1a32(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def pack_section(ppem26_6, layout_bytes):
    """The trailing per-style .cpfont section: header followed by the blob.

    The hash lets the firmware share one copy of the blob between every size
    of a family (each .cpfont file carries the same layout font).
    """
    header = struct.pack(SECTION_HEADER_FORMAT, SECTION_MAGIC, SECTION_VERSION, 0, ppem26_6, len(layout_bytes),
                         fnv1a32(layout_bytes))
    return header + layout_bytes
