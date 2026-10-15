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
    GDEF, GSUB, GPOS) that the shaper reads on the device, plus CPac and
    CPpl, the lookup filters and shaping plans the device would otherwise
    compute itself.

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
SECTION_VERSION = 2  # 2: the layout blob carries CPpl
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

    def u8(self, o):
        if o < 0 or o + 1 > self.length:
            return 0
        return self.data[self.start + o]

    def u16(self, o):
        if o < 0 or o + 2 > self.length:
            return 0
        return int.from_bytes(self.data[self.start + o:self.start + o + 2], "big")

    def s16(self, o):
        v = self.u16(o)
        return v - 0x10000 if v & 0x8000 else v

    def u24(self, o):
        if o < 0 or o + 3 > self.length:
            return 0
        return int.from_bytes(self.data[self.start + o:self.start + o + 3], "big")

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


# --- Shaping plans (the CPpl table) -------------------------------------------
#
# For every script and language system the blob can be shaped in, the plan
# lib/OtShaper would otherwise build on the device: which lookups run, in
# which stages, with which masks. These functions mirror OtPlanBuild.cpp and
# the language-system selection in OtPlan.cpp line for line; the complex_shaper
# host test checks that loading these plans equals building them.

def _tag(t):
    return int.from_bytes(t.encode("latin-1"), "big")


_NOT_FOUND = 0xFFFF
_GLOBAL_BIT_SHIFT = 31
_GLOBAL_BIT_MASK = 1 << _GLOBAL_BIT_SHIFT
_MAX_BITS = 8
_FIRST_FEATURE_BIT = 4
_MAX_FEATURE_VALUE = 255
_MAX_PLANNED_LOOKUPS = 4096
_GSUB, _GPOS = 0, 1

# Feature flags (ff:: in OtPlanBuild.cpp).
_FF_GLOBAL, _FF_HAS_FALLBACK, _FF_MANUAL_ZWNJ, _FF_MANUAL_ZWJ = 0x01, 0x02, 0x04, 0x08
_FF_MANUAL_JOINERS = _FF_MANUAL_ZWNJ | _FF_MANUAL_ZWJ
_FF_RANDOM, _FF_PER_SYLLABLE = 0x20, 0x40
# lookupbits:: in OtPlan.h.
_LB_AUTO_ZWNJ, _LB_AUTO_ZWJ, _LB_RANDOM, _LB_PER_SYLLABLE = 0x01, 0x02, 0x04, 0x08

# ot::ShaperKind and ot::Pause, in declaration order.
_SHAPER_DEFAULT, _SHAPER_INDIC, _SHAPER_USE = 0, 1, 2
(_P_NONE, _P_INDIC_SETUP, _P_INDIC_INITIAL, _P_INDIC_FINAL, _P_USE_SETUP, _P_USE_CLEAR,
 _P_USE_RPHF, _P_USE_PREF, _P_USE_REORDER) = range(9)

# Mirrors SCRIPT_TAGS in lib/OtShaper/OtPlan.cpp (v3, v2, v1), in SCRIPTS order.
_PLAN_SCRIPT_TAGS = {
    "devanagari": ("dev3", "dev2", "deva"), "bengali": ("bng3", "bng2", "beng"),
    "gurmukhi": ("gur3", "gur2", "guru"), "gujarati": ("gjr3", "gjr2", "gujr"),
    "oriya": ("ory3", "ory2", "orya"), "tamil": ("tml3", "tml2", "taml"),
    "telugu": ("tel3", "tel2", "telu"), "kannada": ("knd3", "knd2", "knda"),
    "malayalam": ("mlm3", "mlm2", "mlym"), "sinhala": ("sinh",),
}

# ot::MASKED_FEATURE_TAGS and the would-substitute features, in order.
_MASKED_FEATURES = ("rphf", "pref", "blwf", "abvf", "half", "pstf", "init", "isol", "medi", "fina")
_WOULD_FEATURES = ("rphf", "pref", "blwf", "pstf", "vatu")
_NO_STAGE = 0xFF


def _find_record(records, t):
    lo, hi = 0, records.u16(0) - 1
    while lo <= hi:
        mid = (lo + hi) // 2
        k = records.u32(2 + 6 * mid)
        if t < k:
            hi = mid - 1
        elif t > k:
            lo = mid + 1
        else:
            return mid
    return _NOT_FOUND


def _select_script(layout, tags):
    scripts = layout.offset16(4)
    index, chosen = _NOT_FOUND, 0
    for t in list(tags) + [_tag("DFLT"), _tag("dflt"), _tag("latn")]:
        if index == _NOT_FOUND and t:
            index = _find_record(scripts, t)
            if index != _NOT_FOUND:
                chosen = t
    if index == _NOT_FOUND or index >= scripts.u16(0):
        return _View(), chosen
    return scripts.offset16(2 + 6 * index + 4), chosen


def _select_langsys(script, language_tags):
    """(LangSys table, key) as ot::selectLanguageSystem chooses them."""
    records = script.at(2)
    index, key = _NOT_FOUND, 0
    for t in language_tags[:3]:
        if not t or index != _NOT_FOUND:
            break
        index = _find_record(records, t)
        if index != _NOT_FOUND:
            key = t
    if index == _NOT_FOUND:
        index = _find_record(records, _tag("dflt"))
        if index != _NOT_FOUND:
            key = _tag("dflt")
    if index == _NOT_FOUND:
        return script.offset16(0), key
    if index >= script.u16(2):
        return _View(), key
    return script.offset16(4 + 6 * index + 4), key


def _feature_tag(layout, index):
    features = layout.offset16(6)
    return 0 if index >= features.u16(0) else features.u32(2 + 6 * index)


# Mirrors MAX_CONDITION_EVALUATIONS in OtPlanBuild.cpp.
_MAX_CONDITION_EVALUATIONS = 4096


def _condition_holds(condition, budget, depth=0):
    if depth > 8:
        return False
    budget[0] -= 1
    if budget[0] < 0:
        return False
    fmt = condition.u16(0)
    if fmt == 1:
        return condition.s16(4) <= 0 <= condition.s16(6)
    if fmt == 2:
        return condition.s16(2) > 0
    if fmt in (3, 4):
        is_and = fmt == 3
        for i in range(condition.u8(2)):
            holds = _condition_holds(condition.at(condition.u24(3 + 3 * i)), budget, depth + 1)
            if is_and != holds:
                return holds
        return is_and
    if fmt == 5:
        return not _condition_holds(condition.at(condition.u24(2)), budget, depth + 1)
    return False


def _default_feature_substitution(layout):
    if layout.u16(2) < 1:
        return _View()
    variations = layout.offset32(10)
    budget = [_MAX_CONDITION_EVALUATIONS]
    for i in range(variations.u32(4)):
        record = 8 + 8 * i
        conditions = variations.offset32(record)
        holds = all(_condition_holds(conditions.offset32(2 + 4 * c), budget) for c in range(conditions.u16(0)))
        if budget[0] < 0:
            return _View()
        if holds:
            return variations.offset32(record + 4)
    return _View()


def _feature_table(layout, substitution, index):
    for i in range(substitution.u16(4)):
        if substitution.u16(6 + 6 * i) == index:
            return substitution.offset32(6 + 6 * i + 2)
    features = layout.offset16(6)
    if index >= features.u16(0):
        return _View()
    return features.offset16(2 + 6 * index + 4)


class _FeatureBuilder:
    def __init__(self):
        self.infos = []  # [tag, seq, max_value, flags, default_value, [gsub stage, gpos stage]]
        self.pauses = ([], [])
        self.stage = [0, 0]
        self.simple = True

    def add(self, t, flags=0, value=1):
        default = value if flags & _FF_GLOBAL else 0
        self.infos.append([_tag(t), len(self.infos) + 1, value, flags, default, list(self.stage)])

    def enable(self, t, flags=0, value=1):
        self.add(t, flags | _FF_GLOBAL, value)

    def disable(self, t):
        self.add(t, _FF_GLOBAL, 0)

    def pause(self, table, p):
        self.pauses[table].append(p)
        self.stage[table] += 1

    def sort_and_merge(self):
        if not self.simple:
            self.infos.sort(key=lambda f: (f[0], f[1]))
        merged = []
        for f in self.infos:
            if not merged or merged[-1][0] != f[0]:
                merged.append([f[0], f[1], f[2], f[3], f[4], list(f[5])])
                continue
            m = merged[-1]
            if f[3] & _FF_GLOBAL:
                m[3] |= _FF_GLOBAL
                m[2], m[4] = f[2], f[4]
            else:
                m[3] &= ~_FF_GLOBAL
                m[2] = max(m[2], f[2])
            m[3] |= f[3] & _FF_HAS_FALLBACK
            m[5] = [min(m[5][0], f[5][0]), min(m[5][1], f[5][1])]
        self.infos = merged


def _collect_features(b, shaper):
    """collect_features() for the shaper, as Plan::build requests them."""
    gm = _FF_GLOBAL | _FF_MANUAL_JOINERS
    b.enable("rvrn")
    b.pause(_GSUB, _P_NONE)
    for t in ("ltra", "ltrm"):
        b.enable(t)
    for t in ("frac", "numr", "dnom"):
        b.add(t)
    b.enable("rand", _FF_RANDOM, _MAX_FEATURE_VALUE)
    b.enable("Harf")
    b.enable("HARF")
    if shaper == _SHAPER_INDIC:
        b.simple = False
        b.pause(_GSUB, _P_INDIC_SETUP)
        b.enable("locl", _FF_PER_SYLLABLE)
        b.enable("ccmp", _FF_PER_SYLLABLE)
        b.pause(_GSUB, _P_INDIC_INITIAL)
        for t, flags in (("nukt", gm), ("akhn", gm), ("rphf", _FF_MANUAL_JOINERS), ("rkrf", gm),
                         ("pref", _FF_MANUAL_JOINERS), ("blwf", _FF_MANUAL_JOINERS), ("abvf", _FF_MANUAL_JOINERS),
                         ("half", _FF_MANUAL_JOINERS), ("pstf", _FF_MANUAL_JOINERS), ("vatu", gm), ("cjct", gm)):
            b.add(t, flags | _FF_PER_SYLLABLE)
            b.pause(_GSUB, _P_NONE)
        b.pause(_GSUB, _P_INDIC_FINAL)
        for t, flags in (("init", _FF_MANUAL_JOINERS), ("pres", gm), ("abvs", gm), ("blws", gm), ("psts", gm),
                         ("haln", gm)):
            b.add(t, flags | _FF_PER_SYLLABLE)
    elif shaper == _SHAPER_USE:
        b.simple = False
        b.pause(_GSUB, _P_USE_SETUP)
        for t in ("locl", "ccmp", "nukt"):
            b.enable(t, _FF_PER_SYLLABLE)
        b.enable("akhn", _FF_MANUAL_ZWJ | _FF_PER_SYLLABLE)
        b.pause(_GSUB, _P_USE_CLEAR)
        b.add("rphf", _FF_MANUAL_ZWJ | _FF_PER_SYLLABLE)
        b.pause(_GSUB, _P_USE_RPHF)
        b.pause(_GSUB, _P_USE_CLEAR)
        b.enable("pref", _FF_MANUAL_ZWJ | _FF_PER_SYLLABLE)
        b.pause(_GSUB, _P_USE_PREF)
        for t in ("rkrf", "abvf", "blwf", "half", "pstf", "vatu", "cjct"):
            b.enable(t, _FF_MANUAL_ZWJ | _FF_PER_SYLLABLE)
        b.pause(_GSUB, _P_USE_REORDER)
        b.pause(_GSUB, _P_NONE)
        for t in ("isol", "init", "medi", "fina"):
            b.add(t)
        b.pause(_GSUB, _P_NONE)
        for t in ("abvs", "blws", "haln", "pres", "psts"):
            b.enable(t, _FF_MANUAL_ZWJ)
    b.enable("Buzz")
    b.enable("BUZZ")
    for t in ("abvm", "blwm", "ccmp", "locl"):
        b.add(t, _FF_GLOBAL)
    for t in ("mark", "mkmk"):
        b.add(t, _FF_GLOBAL | _FF_MANUAL_JOINERS)
    for t in ("rlig", "calt", "clig", "curs", "dist"):
        b.add(t, _FF_GLOBAL)
    b.add("kern", _FF_GLOBAL | _FF_HAS_FALLBACK)
    for t in ("liga", "rclt"):
        b.add(t, _FF_GLOBAL)
    if shaper == _SHAPER_INDIC:
        b.disable("liga")
        b.pause(_GSUB, _P_NONE)


def _plan(layouts, script, language_tags):
    """The plan body Plan::load reads, or None when Plan::build would fail."""
    systems = []
    for layout in layouts:
        script_table, chosen = _select_script(layout, [_tag(t) for t in _PLAN_SCRIPT_TAGS[script]])
        lang_sys, _ = _select_langsys(script_table, language_tags)
        required = _NOT_FOUND if lang_sys.empty() else lang_sys.u16(2)
        required_tag = 0 if required == _NOT_FOUND else _feature_tag(layout, required)
        systems.append((chosen, lang_sys, required, required_tag))
    chosen = systems[_GSUB][0]
    if chosen in (_tag("DFLT"), _tag("latn")):
        shaper = _SHAPER_DEFAULT
    elif script == "sinhala" or (chosen & 0xFF) == ord("3"):
        shaper = _SHAPER_USE
    else:
        shaper = _SHAPER_INDIC

    b = _FeatureBuilder()
    _collect_features(b, shaper)
    b.sort_and_merge()

    # mapFeatures()
    def find_feature_index(t, feature_tag):
        lang_sys = systems[t][1]
        for i in range(lang_sys.u16(4)):
            index = lang_sys.u16(6 + 2 * i)
            if _feature_tag(layouts[t], index) == feature_tag:
                return index
        return _NOT_FOUND

    features = []  # [tag, index[2], stage[2], lookup flags, shift, mask]
    global_mask = _GLOBAL_BIT_MASK
    next_bit = _FIRST_FEATURE_BIT
    required_stage = [0, 0]
    for tag, _, max_value, flags, default, stage in b.infos:
        uses_global_bit = bool(flags & _FF_GLOBAL) and max_value == 1
        bits = 0 if uses_global_bit else min(_MAX_BITS, max_value.bit_length())
        if not max_value or next_bit + bits >= _GLOBAL_BIT_SHIFT:
            continue
        index = [_NOT_FOUND, _NOT_FOUND]
        for t in (_GSUB, _GPOS):
            if systems[t][3] == tag:
                required_stage[t] = stage[t]
            index[t] = find_feature_index(t, tag)
        if index == [_NOT_FOUND, _NOT_FOUND] and not flags & _FF_HAS_FALLBACK:
            continue
        lookup_flags = ((0 if flags & _FF_MANUAL_ZWNJ else _LB_AUTO_ZWNJ) |
                        (0 if flags & _FF_MANUAL_ZWJ else _LB_AUTO_ZWJ) |
                        (_LB_RANDOM if flags & _FF_RANDOM else 0) |
                        (_LB_PER_SYLLABLE if flags & _FF_PER_SYLLABLE else 0))
        if uses_global_bit:
            shift, mask = _GLOBAL_BIT_SHIFT, _GLOBAL_BIT_MASK
        else:
            shift, mask = next_bit, (1 << (next_bit + bits)) - (1 << next_bit)
            next_bit += bits
            global_mask |= (default << shift) & mask
        features.append([tag, index, [stage[0] & 0xFF, stage[1] & 0xFF], lookup_flags, shift, mask])
    if b.simple:
        features.sort(key=lambda f: f[0])
    b.pause(_GSUB, _P_NONE)
    b.pause(_GPOS, _P_NONE)

    # planLookups()
    tables = []
    for t in (_GSUB, _GPOS):
        layout = layouts[t]
        substitution = _default_feature_substitution(layout)
        lookup_count = layout.offset16(8).u16(0)

        def feature_lookups(index):
            return _View() if index == _NOT_FOUND else _feature_table(layout, substitution, index)

        total = feature_lookups(systems[t][2]).u16(2) + sum(feature_lookups(f[1][t]).u16(2) for f in features)
        if total > _MAX_PLANNED_LOOKUPS:
            return None
        lookups, stages = [], []

        def add_lookups(index, mask, flags):
            feature = feature_lookups(index)
            for i in range(feature.u16(2)):
                lookup = feature.u16(4 + 2 * i)
                if lookup < lookup_count:
                    lookups.append([lookup, flags, mask])

        last = 0
        for stage in range(b.stage[t]):
            if systems[t][2] != _NOT_FOUND and required_stage[t] == stage:
                add_lookups(systems[t][2], _GLOBAL_BIT_MASK, _LB_AUTO_ZWNJ | _LB_AUTO_ZWJ)
            for f in features:
                if f[2][t] == stage:
                    add_lookups(f[1][t], f[5], f[3])
            if last + 1 < len(lookups):
                lookups[last:] = sorted(lookups[last:], key=lambda l: l[0])  # stable
                merged = lookups[:last]
                for l in lookups[last:]:
                    if len(merged) > last and merged[-1][0] == l[0]:
                        m = merged[-1]
                        joiners = _LB_AUTO_ZWNJ | _LB_AUTO_ZWJ
                        m[2] |= l[2]
                        m[1] = (m[1] & ~joiners) | (m[1] & l[1] & joiners)
                    else:
                        merged.append(list(l))
                lookups = merged
            last = len(lookups)
            stages.append((last, b.pauses[t][stage]))
        tables.append((stages, lookups))

    def one_mask(tag):
        for f in features:
            if f[0] == tag:
                return (1 << f[4]) & f[5]
        return 0

    def gsub_stage(tag):
        stage = _NO_STAGE
        for f in features:
            if f[0] == tag:
                stage = f[2][_GSUB]
        return stage

    body = struct.pack(">IB3xI", chosen, shaper, global_mask)
    body += b"".join(struct.pack(">I", one_mask(_tag(t))) for t in _MASKED_FEATURES)
    body += bytes(gsub_stage(_tag(t)) for t in _WOULD_FEATURES) + b"\0\0\0"
    for stages, lookups in tables:
        body += struct.pack(">HH", len(stages), len(lookups))
        body += b"".join(struct.pack(">HBx", last, pause) for last, pause in stages)
        body += b"".join(struct.pack(">HBxI", index, flags, mask) for index, flags, mask in lookups)
    return body


def compiled_plans(layout_bytes, scripts):
    """The CPpl table for a layout font (see docs/file-formats.md).

    One plan per script and pair of language systems (GSUB's and GPOS's) a
    request of up to three language tags can select; plans that come out
    byte-identical are stored once.
    """
    from itertools import permutations

    layouts = []
    for tag in (b"GSUB", b"GPOS"):
        layout = _View(_sfnt_table(layout_bytes, tag))
        layouts.append(layout if layout.u16(0) == 1 else _View())
    records, bodies = [], []
    for script in scripts:
        script_index = list(SCRIPTS).index(script)
        script_tables = [_select_script(layout, [_tag(t) for t in _PLAN_SCRIPT_TAGS[script]])[0] for layout in layouts]
        language_tags = sorted({script_table.at(2).u32(2 + 6 * i) for script_table in script_tables
                                for i in range(script_table.at(2).u16(0))})
        requests = [()] + [r for n in (1, 2, 3) for r in permutations(language_tags, n)]
        keyed = {}
        for request in requests:
            key = tuple(_select_langsys(script_table, list(request))[1] for script_table in script_tables)
            if key not in keyed:
                keyed[key] = _plan(layouts, script, list(request))
        for (gsub_key, gpos_key), body in sorted(keyed.items()):
            if body is None:
                continue
            if body not in bodies:
                bodies.append(body)
            records.append((script_index, gsub_key, gpos_key, bodies.index(body)))
    header_bytes = 4 + 14 * len(records)
    offsets, at = [], header_bytes
    for body in bodies:
        offsets.append(at)
        at += len(body)
    out = struct.pack(">HH", 1, len(records))
    out += b"".join(struct.pack(">BxIII", s, g, p, offsets[i]) for s, g, p, i in records)
    return out + b"".join(bodies)


def _add_tables(layout_bytes, tables):
    """layout_bytes with `tables` ({tag: bytes}) added; every other table byte for byte."""
    from fontTools.ttLib import TTFont, newTable

    font = TTFont(io.BytesIO(layout_bytes), lazy=True, recalcTimestamp=False)
    for tag, data in tables.items():
        table = newTable(tag)
        table.data = data
        font[tag] = table
    buf = io.BytesIO()
    font.save(buf, reorderTables=True)
    out = buf.getvalue()
    for tag in (b"GSUB", b"GPOS", b"GDEF", b"cmap", b"hmtx"):
        if _sfnt_table(out, tag) != _sfnt_table(layout_bytes, tag):
            raise AssertionError(f"{tag} changed while adding {', '.join(tables)}")
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
    stripped = _strip_to_layout(layout)
    plans = compiled_plans(stripped, scripts)
    layout_bytes = _add_tables(stripped, {"CPpl": plans})
    with_filters = _add_tables(stripped, {"CPpl": plans, "CPac": lookup_filters(stripped)})
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
