"""How often does fontconvert_sdcard.py's 'sum every subtable' kerning differ from
OpenType's 'first matching subtable per lookup' rule? Uses the repo's own
extraction for the summed side.  python -I kern_semantics.py <repo> <size> <font>..."""
import contextlib, io, os, sys
repo, size, fonts = sys.argv[1], int(sys.argv[2]), sys.argv[3:]
sys.path.insert(0, os.path.join(repo, "lib", "EpdFont", "scripts"))
sys.dont_write_bytecode = True
import fontconvert_sdcard as fc
from fontTools.ttLib import TTFont

def first_match(font_path, cps, ppem):
    f = TTFont(font_path); upem = f['head'].unitsPerEm; cmap = f.getBestCmap()
    g2c = {}
    for cp in cps:
        if cp in cmap: g2c.setdefault(cmap[cp], []).append(cp)
    gpos = f['GPOS'].table if 'GPOS' in f else None
    raw = {}
    if gpos:
        idx = sorted({i for fr in gpos.FeatureList.FeatureRecord if fr.FeatureTag == 'kern' for i in fr.Feature.LookupListIndex})
        for li in idx:
            lk = gpos.LookupList.Lookup[li]
            done = set()  # pairs already matched by an earlier subtable of this lookup
            for st in lk.SubTable:
                a = st.ExtSubTable if lk.LookupType == 9 else st
                if getattr(st, 'ExtensionLookupType', lk.LookupType) != 2: continue
                cov = set(a.Coverage.glyphs)
                if a.Format == 1:
                    for i, lg in enumerate(a.Coverage.glyphs):
                        if lg not in g2c: continue
                        for pvr in a.PairSet[i].PairValueRecord:
                            rg = pvr.SecondGlyph
                            if rg not in g2c or (lg, rg) in done: continue
                            done.add((lg, rg))
                            xa = (getattr(pvr.Value1, 'XAdvance', 0) or 0) if pvr.Value1 else 0
                            if xa: raw[(lg, rg)] = raw.get((lg, rg), 0) + xa
                else:
                    cd1 = a.ClassDef1.classDefs if a.ClassDef1 else {}
                    cd2 = a.ClassDef2.classDefs if a.ClassDef2 else {}
                    for lg in g2c:
                        if lg not in cov: continue
                        rec = a.Class1Record[cd1.get(lg, 0)]
                        for rg in g2c:
                            if (lg, rg) in done: continue
                            done.add((lg, rg))  # format 2 matches every second glyph
                            v = rec.Class2Record[cd2.get(rg, 0)].Value1
                            xa = (getattr(v, 'XAdvance', 0) or 0) if v else 0
                            if xa: raw[(lg, rg)] = raw.get((lg, rg), 0) + xa
    has_kern = 'kern' in f
    out = {}
    for (lg, rg), du in raw.items():
        adj = fc.fp4_from_design_units(du, ppem / upem)
        if adj:
            for l in g2c[lg]:
                for r in g2c[rg]: out[(l, r)] = adj
    return out, has_kern, bool(gpos)

for path in fonts:
    cps = set()
    for s, e in fc.resolve_intervals("builtin,latin-cyrillic,greek-letters,musical-symbols"):
        cps.update(range(s, e + 1))
    ppem = size * 150.0 / 72.0
    with contextlib.redirect_stderr(io.StringIO()):
        summed = fc.extract_kerning_fonttools(path, cps, ppem)
    spec, has_kern, has_gpos = first_match(path, cps, ppem)
    keys = set(summed) | set(spec)
    diff = [k for k in keys if summed.get(k, 0) != spec.get(k, 0)]
    ex = sorted(diff)[:3]
    print(f"{os.path.basename(path)} @{size}: kern table={has_kern} GPOS={has_gpos}; pairs summed={len(summed)} "
          f"first-match={len(spec)}; differing={len(diff)} e.g. " +
          ", ".join(f"{chr(l)}{chr(r)} {summed.get((l,r),0)}/{spec.get((l,r),0)}" for l, r in ex))
