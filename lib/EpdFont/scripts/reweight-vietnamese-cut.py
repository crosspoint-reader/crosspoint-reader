#!/usr/bin/env python3
"""Re-weight the Vietnamese Ubuntu cut to another weight of the same family.

The built-in UI fonts need U+1EA0-U+1EF9, which the Ubuntu family does not
carry. `Ubuntu-Vietnamese-Regular.ttf` supplies them as composites of Ubuntu
base letters plus accents. This rebuilds that cut against a different weight:
base letters and the five accents Ubuntu actually ships are taken from the
target weight, accents are re-centred for the target's advance widths, and the
two marks Ubuntu has no glyph for (hook above, dot below) are carried over.

Usage: make_viet_cut.py <cut.ttf> <source-weight.ttf> <target-weight.ttf> <out.ttf>
"""
import sys
from fontTools.ttLib import TTFont
from fontTools.pens.ttGlyphPen import TTGlyphPen

cut_path, src_path, dst_path, out_path = sys.argv[1:5]
# recalcTimestamp=False keeps the source cut's head.modified, so regenerating
# the same inputs produces a byte-identical file.
V = TTFont(cut_path, recalcTimestamp=False)
SRC = TTFont(src_path)
DST = TTFont(dst_path)

vglyf, vhmtx = V["glyf"], V["hmtx"]
src_glyf, dst_glyf = SRC["glyf"], DST["glyf"]


def fingerprint(font, name):
    """Coordinates of a simple glyph, used to identify subset-renamed glyphs."""
    g = font["glyf"][name]
    if g.isComposite() or g.numberOfContours == 0:
        return None
    coords, _, ends = g.getCoordinates(font["glyf"])
    return (tuple(map(tuple, coords)), tuple(ends))


src_by_shape = {}
for name in SRC.getGlyphOrder():
    key = fingerprint(SRC, name)
    if key:
        src_by_shape.setdefault(key, name)

dst_names = set(DST.getGlyphOrder())

# Map each simple glyph of the cut onto its counterpart in the target weight.
replacement = {}
for name in V.getGlyphOrder():
    key = fingerprint(V, name)
    if not key:
        continue
    src_name = src_by_shape.get(key)
    if src_name and src_name in dst_names:
        replacement[name] = src_name

carried = [n for n in V.getGlyphOrder()
           if fingerprint(V, n) and n not in replacement]

# Advance delta per replaced glyph, used to re-centre accents.
delta = {}
for name, src_name in replacement.items():
    old = vhmtx[name][0]
    new = DST["hmtx"][src_name][0]
    delta[name] = new - old

# Swap outlines, decomposing so nothing references glyphs absent from the cut.
for name, src_name in replacement.items():
    pen = TTGlyphPen(None)
    DST.getGlyphSet()[src_name].draw(pen)
    vglyf[name] = pen.glyph()
    vhmtx[name] = (DST["hmtx"][src_name][0], DST["hmtx"][src_name][1])


def depth(name, seen=()):
    g = vglyf[name]
    if not g.isComposite() or name in seen:
        return 0
    return 1 + max(depth(c.glyphName, (*seen, name)) for c in g.components)


# Shallowest composites first, so a chained base is already updated.
for name in sorted((n for n in V.getGlyphOrder() if vglyf[n].isComposite()),
                   key=depth):
    g = vglyf[name]
    base = g.components[0].glyphName
    shift = delta.get(base, 0)
    for comp in g.components[1:]:
        comp.x += round(shift / 2)
    old_adv = vhmtx[name][0]
    new_adv = vhmtx[base][0]
    vhmtx[name] = (new_adv, vhmtx[name][1])
    delta[name] = new_adv - old_adv

V.save(out_path)

print(f"replaced {len(replacement)} glyphs from {dst_path}")
print(f"carried over unchanged: {carried}")
