"""OpenType shaping data for complex scripts in .cpfont files.

The firmware shapes complex-script runs (Bengali today) with HarfBuzz at
runtime. HarfBuzz needs the font's layout tables, and the renderer needs a
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

# Mirrors lib/EpdFont/ShapingTokens.h.
GLYPH_TOKEN_BASE = 0xF0000
GLYPH_TOKEN_MAX_GID = 0xDFFF

SECTION_MAGIC = b"CPSH"
SECTION_VERSION = 1
SECTION_HEADER_FORMAT = "<4sHHIII"  # magic, version, reserved, ppem26_6, blobLength, blobHash
SECTION_HEADER_SIZE = struct.calcsize(SECTION_HEADER_FORMAT)

# Characters that belong to a shaped run. The dotted circle is inserted by
# HarfBuzz for broken clusters; ZWJ/ZWNJ steer conjunct formation.
SCRIPT_UNICODES = {
    "bengali": [0x0020] + list(range(0x0980, 0x0A00)) + [0x0964, 0x0965, 0x200C, 0x200D, 0x25CC],
}

_LAYOUT_TABLES = {"head", "hhea", "maxp", "hmtx", "cmap", "GDEF", "GSUB", "GPOS"}


def font_supports_script(fontfile, script):
    """True when the font maps the script's letters and carries a GSUB table."""
    from fontTools.ttLib import TTFont

    font = TTFont(fontfile, fontNumber=0, lazy=True)
    if "GSUB" not in font:
        return False
    cmap = font.getBestCmap() or {}
    letters = [cp for cp in SCRIPT_UNICODES[script] if 0x0995 <= cp <= 0x09B9]
    return sum(1 for cp in letters if cp in cmap) >= len(letters) // 2


def _subset(fontfile, unicodes):
    from fontTools import subset
    from fontTools.ttLib import TTFont

    font = TTFont(fontfile, fontNumber=0)
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.layout_scripts = ["beng", "bng2", "DFLT"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.glyph_names = True
    opts.hinting = True
    opts.legacy_kern = False
    opts.drop_tables += ["morx", "mort", "kerx", "feat", "trak", "ankr", "meta", "DSIG"]
    subsetter = subset.Subsetter(opts)
    subsetter.populate(unicodes=unicodes)
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


def build(fontfile, script="bengali"):
    """Returns (render_font_bytes, layout_blob_bytes, glyph_count).

    render_font_bytes is the outline subset FreeType rasterizes by glyph ID.
    """
    font = _subset(fontfile, SCRIPT_UNICODES[script])
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
