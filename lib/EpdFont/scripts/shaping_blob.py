"""OpenType shaping data for complex scripts in .cpfont files.

The firmware shapes complex-script runs (the Indic scripts in SCRIPTS) with
HarfBuzz at runtime. HarfBuzz needs the font's layout tables, and the renderer needs a
bitmap for every glyph those tables can produce — conjuncts, reph, below-base
forms — most of which have no Unicode codepoint at all.

This module builds both from one fontTools subset of the source font:

  * the subset, WITH outlines, is what FreeType rasterizes by glyph ID; each
    glyph is stored in the .cpfont at codepoint GLYPH_TOKEN_BASE + glyph ID;
  * the same subset with outlines, hinting and naming stripped is the
    "layout blob": a minimal OpenType font (head, hhea, maxp, hmtx, cmap,
    GDEF, GSUB, GPOS) that HarfBuzz reads on the device.

Both share one glyph order, so the glyph IDs HarfBuzz emits index the
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
# (which steer conjunct formation) and the dotted circle HarfBuzz inserts for
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

    font = TTFont(fontfile, fontNumber=0)
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
    """Reduce a subset font to the tables HarfBuzz shapes with."""
    from fontTools.ttLib import newTable

    for tag in list(font.keys()):
        if tag != "GlyphOrder" and tag not in _LAYOUT_TABLES:
            del font[tag]
    # Outlines are gone, so maxp becomes the CFF-style 0.5 table (glyph count
    # only), which HarfBuzz accepts without glyf/loca.
    num_glyphs = font["maxp"].numGlyphs
    maxp = newTable("maxp")
    maxp.tableVersion = 0x00005000
    maxp.numGlyphs = num_glyphs
    font["maxp"] = maxp
    buf = io.BytesIO()
    font.save(buf, reorderTables=True)
    return buf.getvalue()


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

    layout = TTFont(io.BytesIO(render_bytes))
    layout_bytes = _strip_to_layout(layout)
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
