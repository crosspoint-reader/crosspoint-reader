#!/usr/bin/env python3
"""Host unit tests for 4-bit fontconvert_sdcard.py quantization, counter guard, and outline stem anchoring."""

from __future__ import annotations

import importlib.util
import math
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = ROOT / "lib" / "EpdFont" / "scripts"
sys.path.insert(0, str(SCRIPTS_DIR))

import fontconvert_sdcard as fcs  # noqa: E402
from fontconvert_sdcard import (  # noqa: E402
    MAX_CHAIN_SEARCH_STEPS,
    MAX_STEM_ANCHOR_CONTOURS,
    MAX_STEM_ANCHOR_POINTS,
    MIN_COUNTER_GAP_26_6,
    MIN_COUNTER_GAP_RATIO,
    _is_bicameral_stem_anchor_cp,
    _is_italic_face,
    _probe_stem_anchor_zones,
    apply_stem_anchor_pass,
    build_quant_lut,
    pack_bitmap_4b,
    rasterize_font_style,
)

try:
    import freetype  # type: ignore
    from fontTools.fontBuilder import FontBuilder  # type: ignore
    from fontTools.pens.ttGlyphPen import TTGlyphPen  # type: ignore

    HAS_FONT_DEPS = True
except ImportError:
    HAS_FONT_DEPS = False


def _unpack_4b(packed: bytes, width: int, rows: int) -> list[list[int]]:
    vals: list[int] = []
    for b in packed:
        vals.append((b >> 4) & 0x0F)
        vals.append(b & 0x0F)
    total = width * rows
    vals = vals[:total]
    return [vals[y * width : (y + 1) * width] for y in range(rows)]


class TestBuildQuantLut(unittest.TestCase):
    def test_2bpp_default_and_halo_clip_preserve_legacy_thresholds(self) -> None:
        lut_off = build_quant_lut(2, gamma=1.0, halo_clip=False)
        lut_on = build_quant_lut(2, gamma=1.0, halo_clip=True)
        self.assertEqual(len(lut_off), 256)
        self.assertEqual(lut_off, lut_on)
        self.assertEqual(lut_off[0], 0)
        self.assertEqual(lut_off[63], 0)
        self.assertEqual(lut_off[64], 1)
        self.assertEqual(lut_off[127], 1)
        self.assertEqual(lut_off[128], 2)
        self.assertEqual(lut_off[191], 2)
        self.assertEqual(lut_off[192], 3)
        self.assertEqual(lut_off[255], 3)

    def test_4bpp_halo_clip_false_vs_true(self) -> None:
        for gamma in (1.0, 1.2):
            lut_raw = build_quant_lut(4, gamma=gamma, halo_clip=False)
            lut_clip = build_quant_lut(4, gamma=gamma, halo_clip=True)
            self.assertEqual(len(lut_raw), 256)
            self.assertEqual(len(lut_clip), 256)

            # Endpoints and monotonicity
            self.assertEqual(lut_raw[0], 0)
            self.assertEqual(lut_raw[255], 15)
            self.assertEqual(lut_clip[0], 0)
            self.assertEqual(lut_clip[255], 15)
            for i in range(255):
                self.assertLessEqual(lut_raw[i], lut_raw[i + 1])
                self.assertLessEqual(lut_clip[i], lut_clip[i + 1])

            # Unclipped LUT maps coverage 9..15 to ink 1 and 240..245 to ink 14 at gamma=1.0
            if gamma == 1.0:
                self.assertEqual(lut_raw[12], 1)
                self.assertEqual(lut_raw[15], 1)
                self.assertEqual(lut_raw[240], 14)

            # halo_clip=True zeroes <= 15/255 outer halo and saturates >= 240/255 solid core,
            # while leaving interior tones 16..239 bit-identical.
            for i in range(16):
                self.assertEqual(lut_clip[i], 0, f"gamma={gamma} i={i}")
            self.assertGreaterEqual(lut_clip[16], 1)
            for i in range(16, 240):
                self.assertEqual(lut_clip[i], lut_raw[i], f"gamma={gamma} i={i}")
            for i in range(240, 256):
                self.assertEqual(lut_clip[i], 15, f"gamma={gamma} i={i}")

    def test_invalid_arguments_raise(self) -> None:
        with self.assertRaises(ValueError):
            build_quant_lut(1)
        with self.assertRaises(ValueError):
            build_quant_lut(8)
        for bad_gamma in (0.0, -0.5, math.nan, math.inf):
            with self.assertRaises(ValueError):
                build_quant_lut(4, gamma=bad_gamma)


class TestPackBitmap4b(unittest.TestCase):
    def setUp(self) -> None:
        # Identity 4bpp mapping: input byte k * 17 -> nibble k (0..15)
        self.lut4 = build_quant_lut(4, gamma=1.0, halo_clip=False)

    def test_empty_odd_width_and_negative_pitch(self) -> None:
        self.assertEqual(pack_bitmap_4b(b"", 0, 0, 0, self.lut4), b"")
        # 3x2 pixels with row padding (pitch = 4):
        # row 0: [0, 5, 10], row 1: [15, 2, 8]
        raw_top_down = bytes([0, 5 * 17, 10 * 17, 99, 15 * 17, 2 * 17, 8 * 17, 99])
        packed_pos = pack_bitmap_4b(raw_top_down, 3, 2, 4, self.lut4, counter_guard=False)
        self.assertEqual(_unpack_4b(packed_pos, 3, 2), [[0, 5, 10], [15, 2, 8]])

        # Bottom-up buffer (pitch = -4): stored row 0 in memory is bottom row
        raw_bottom_up = bytes([15 * 17, 2 * 17, 8 * 17, 99, 0, 5 * 17, 10 * 17, 99])
        packed_neg = pack_bitmap_4b(raw_bottom_up, 3, 2, -4, self.lut4, counter_guard=False)
        self.assertEqual(packed_neg, packed_pos)

    def test_counter_guard_clears_only_faint_3_wall_tight_counters(self) -> None:
        # 5x3 grid:
        #   col 1 (y=1, x=1): ink=2 surrounded by 3 dark walls (left=12, top=14, right=11, bottom=0) -> cleared to 0
        #   col 3 (y=1, x=3): ink=2 with only 2 dark walls (left=11, top=13, right=0, bottom=0) -> kept as 2
        #   col 1 (y=2, x=1): ink=3 surrounded by 3 dark walls -> kept as 3 (guard only clears <= 2)
        grid = [
            [0, 14, 0, 13, 0],
            [12, 2, 11, 2, 0],
            [12, 3, 12, 0, 0],
        ]
        buf = bytes(v * 17 for row in grid for v in row)

        out_unguarded = _unpack_4b(pack_bitmap_4b(buf, 5, 3, 5, self.lut4, counter_guard=False), 5, 3)
        out_guarded = _unpack_4b(pack_bitmap_4b(buf, 5, 3, 5, self.lut4, counter_guard=True), 5, 3)

        self.assertEqual(out_unguarded, grid)
        self.assertEqual(out_guarded[1][1], 0, "3-wall tight counter haze (ink=2) must clear to 0")
        self.assertEqual(out_guarded[1][3], 2, "2-wall outer corner (ink=2) must remain untouched")
        self.assertEqual(out_guarded[2][1], 3, "ink=3 pixel must not be cleared by counter guard")

        # Also verify a narrow 2x3 vertical 3-wall pocket clears cleanly
        vgrid = [
            [15, 12],
            [14, 1],
            [15, 11],
        ]
        vbuf = bytes(v * 17 for row in vgrid for v in row)
        vout = _unpack_4b(pack_bitmap_4b(vbuf, 2, 3, 2, self.lut4, counter_guard=True), 2, 3)
        self.assertEqual(vout[1][1], 0)


def _add_cw_rect(pen: "TTGlyphPen", x0: int, y0: int, x1: int, y1: int) -> None:
    """TrueType outer contour (clockwise in Y-up coordinates)."""
    pen.moveTo((x0, y0))
    pen.lineTo((x0, y1))
    pen.lineTo((x1, y1))
    pen.lineTo((x1, y0))
    pen.closePath()


def _add_ccw_rect(pen: "TTGlyphPen", x0: int, y0: int, x1: int, y1: int) -> None:
    """TrueType inner counter contour (counter-clockwise in Y-up coordinates)."""
    pen.moveTo((x0, y0))
    pen.lineTo((x1, y0))
    pen.lineTo((x1, y1))
    pen.lineTo((x0, y1))
    pen.closePath()


def _build_synthetic_4bit_ttf(path: Path) -> None:
    """Build a minimal 1000-UPM TrueType font with flat zone probes plus 'n', 'm', and 'e'."""
    upm = 1000
    cap_h = 700
    x_h = 500

    glyph_order = [".notdef", "space", "bar"] + [f"g_{c}" for c in "BEHTZwxyznme"]
    cmap: dict[int, str] = {ord(" "): "space", ord("|"): "bar"}
    for c in "BEHTZwxyznme":
        cmap[ord(c)] = f"g_{c}"

    glyf = {}
    hmtx: dict[str, tuple[int, int]] = {}

    def make_glyph(name: str, adv: int, draw_fn) -> None:
        pen = TTGlyphPen(None)
        draw_fn(pen)
        g = pen.glyph()
        glyf[name] = g
        lsb = x0_min[0] if (x0_min := [pt[0] for pt in g.coordinates]) else 0
        hmtx[name] = (adv, lsb)

    make_glyph(".notdef", 500, lambda p: _add_cw_rect(p, 50, 0, 450, cap_h))
    make_glyph("space", 260, lambda p: None)
    make_glyph("bar", 240, lambda p: _add_cw_rect(p, 85, -150, 155, 800))

    # Flat cap-height and x-height zone probe glyphs
    for c in "BEHTZ":
        make_glyph(f"g_{c}", 560, lambda p: _add_cw_rect(p, 70, 0, 490, cap_h))
    for c in "wxyz":
        make_glyph(f"g_{c}", 480, lambda p: _add_cw_rect(p, 60, 0, 420, x_h))

    # 'n': two subpixel-positioned vertical stems + <44/64px top overshoot arch (y = 516 UPM -> +0.4px at 25ppem)
    def draw_n(pen: "TTGlyphPen") -> None:
        pen.moveTo((68, 0))
        pen.lineTo((68, x_h))
        pen.lineTo((260, 516))
        pen.lineTo((452, x_h))
        pen.lineTo((452, 0))
        pen.lineTo((366, 0))
        pen.lineTo((366, 410))
        pen.lineTo((154, 410))
        pen.lineTo((154, 0))
        pen.closePath()

    make_glyph("g_n", 520, draw_n)

    # 'm': three equal-pitch vertical stems (pitch = 314 UPM -> 7.85px at 25ppem; width = 82 UPM)
    def draw_m(pen: "TTGlyphPen") -> None:
        s0_l, s0_r = 68, 150
        s1_l, s1_r = 382, 464
        s2_l, s2_r = 696, 778
        pen.moveTo((s0_l, 0))
        pen.lineTo((s0_l, x_h))
        pen.lineTo((225, 516))
        pen.lineTo((s1_l, x_h))
        pen.lineTo((539, 516))
        pen.lineTo((s2_r, x_h))
        pen.lineTo((s2_r, 0))
        pen.lineTo((s2_l, 0))
        pen.lineTo((s2_l, 410))
        pen.lineTo((s1_r, 410))
        pen.lineTo((s1_r, 0))
        pen.lineTo((s1_l, 0))
        pen.lineTo((s1_l, 410))
        pen.lineTo((s0_r, 410))
        pen.lineTo((s0_r, 0))
        pen.closePath()

    make_glyph("g_m", 846, draw_m)

    # 'e': outer bowl with slight baseline (-16 UPM) & x-height (+16 UPM) overshoots,
    # horizontal crossbar (y = 230..294 UPM), and CCW upper eye counter (y = 294..425 UPM).
    def draw_e(pen: "TTGlyphPen") -> None:
        # Outer contour (CW)
        pen.moveTo((56, 120))
        pen.lineTo((56, 390))
        pen.lineTo((250, 516))
        pen.lineTo((444, 390))
        pen.lineTo((444, 230))
        pen.lineTo((142, 230))
        pen.lineTo((142, 85))
        pen.lineTo((255, -16))
        pen.lineTo((430, 60))
        pen.lineTo((430, 120))
        pen.lineTo((348, 120))
        pen.lineTo((255, 68))
        pen.lineTo((142, 120))
        pen.closePath()
        # Inner eye counter (CCW, bottom edge y=294 forms crossbar with y=230)
        _add_ccw_rect(pen, 142, 294, 358, 425)

    make_glyph("g_e", 500, draw_e)

    fb = FontBuilder(upm, isTTF=True)
    fb.setupGlyphOrder(glyph_order)
    fb.setupCharacterMap(cmap)
    fb.setupGlyf(glyf)
    fb.setupHorizontalMetrics(hmtx)
    fb.setupHorizontalHeader(ascent=800, descent=-200)
    fb.setupNameTable({"familyName": "StemAnchorTest", "styleName": "Regular"})
    fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, sxHeight=x_h, sCapHeight=cap_h)
    fb.setupPost()
    fb.save(str(path))


class TestStemAnchorPass(unittest.TestCase):
    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for outline stem-anchor tests")
    def test_stem_anchor_pass_on_n_m_e_and_rasterizer_integration(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ttf_path = Path(tmp) / "StemAnchorTest-Regular.ttf"
            _build_synthetic_4bit_ttf(ttf_path)

            face = freetype.Face(str(ttf_path))
            pt_size = 12
            face.set_char_size(pt_size << 6, pt_size << 6, 150, 150)
            ppem = pt_size * 150.0 / 72.0  # 25.0 ppem -> 1 UPM = 1.6 units in 26.6 space
            outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT

            zones = _probe_stem_anchor_zones(face, outline_flags)
            self.assertEqual(zones["base_flat"], 0)
            self.assertGreater(zones["x_flat"], 0)
            self.assertGreater(zones["cap_flat"], zones["x_flat"])
            self.assertEqual(zones["x_flat"] % 64, 0)
            self.assertEqual(zones["cap_flat"] % 64, 0)

            def load_and_anchor(ch: str):
                gi = face.get_char_index(ord(ch))
                self.assertGreater(gi, 0)
                face.load_glyph(gi, outline_flags)
                ol = face.glyph.outline
                raw = ol._FT_Outline
                before_pts = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]
                before_contours = [raw.contours[c] for c in range(ol.n_contours)]
                before_tags = [raw.tags[i] & 1 for i in range(ol.n_points)]
                before_orient = freetype.raw.FT_Outline_Get_Orientation(freetype.ctypes.byref(raw))

                apply_stem_anchor_pass(face, ppem, zones, snap_x=True)

                after_pts = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]
                after_contours = [raw.contours[c] for c in range(ol.n_contours)]
                after_tags = [raw.tags[i] & 1 for i in range(ol.n_points)]
                after_orient = freetype.raw.FT_Outline_Get_Orientation(freetype.ctypes.byref(raw))

                self.assertEqual(before_contours, after_contours, f"contour topology changed for {ch!r}")
                self.assertEqual(before_tags, after_tags, f"curve tags changed for {ch!r}")
                self.assertEqual(before_orient, after_orient, f"outline orientation flipped for {ch!r}")

                # Ensure FreeType renders the anchored outline cleanly without error or empty output.
                face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
                self.assertGreater(face.glyph.bitmap.width, 0)
                self.assertGreater(face.glyph.bitmap.rows, 0)
                return before_pts, after_pts, after_contours

            # 1. 'n': two stems anchored to 64-unit grid on at least one edge, width preserved, x-overshoot collapsed
            n_before, n_after, _ = load_and_anchor("n")
            n_l0_w_before = n_before[8][0] - n_before[0][0]
            n_l0_w_after = n_after[8][0] - n_after[0][0]
            self.assertEqual(n_l0_w_after, n_l0_w_before)
            self.assertTrue(n_after[0][0] % 64 == 0 or n_after[8][0] % 64 == 0)
            self.assertGreater(n_before[2][1], zones["x_flat"])
            self.assertEqual(n_after[2][1], zones["x_flat"])

            # 2. 'm': three stems locked to equal integer-pixel pitch with preserved stem widths
            _, m_after, _ = load_and_anchor("m")
            s0_l, s0_r = m_after[0][0], m_after[14][0]
            s1_l, s1_r = m_after[11][0], m_after[9][0]
            s2_l, s2_r = m_after[7][0], m_after[6][0]
            pitch_01 = s1_l - s0_l
            pitch_12 = s2_l - s1_l
            self.assertEqual(pitch_01, pitch_12, "3-stem 'm' must lock to uniform inter-stem pitch")
            self.assertEqual(pitch_01 % 64, 0, "inter-stem pitch must be an exact integer pixel multiple")
            self.assertEqual(s0_r - s0_l, s1_r - s1_l)
            self.assertEqual(s1_r - s1_l, s2_r - s2_l)

            # 3. 'e': 2 contours intact, crossbar anchored to 64-unit grid, inner eye counter stays open,
            # and both baseline/x-height overshoots collapse cleanly.
            e_before, e_after, e_contours = load_and_anchor("e")
            self.assertEqual(len(e_contours), 2)
            c0_end = e_contours[0]
            eye_pts = e_after[c0_end + 1 :]
            eye_ys = [p[1] for p in eye_pts]
            eye_xs = [p[0] for p in eye_pts]
            self.assertGreater(max(eye_ys) - min(eye_ys), 96, "'e' inner eye counter must remain open vertically")
            self.assertGreater(max(eye_xs) - min(eye_xs), 128, "'e' inner eye counter must remain open horizontally")
            cross_bot_y = e_after[4][1]
            cross_top_y = min(eye_ys)
            self.assertTrue(cross_bot_y % 64 == 0 or cross_top_y % 64 == 0)
            self.assertGreater(e_before[2][1], zones["x_flat"])
            self.assertEqual(e_after[2][1], zones["x_flat"])
            self.assertLess(e_before[7][1], zones["base_flat"])
            self.assertEqual(e_after[7][1], zones["base_flat"])

            # 4. End-to-end rasterize_font_style smoke check with --4bit --gamma 1.10 --stem-anchor --halo-clip
            sd = rasterize_font_style(
                str(ttf_path),
                pt_size,
                [(ord(" "), ord("z"))],
                style_id=0,
                four_bit=True,
                gamma=1.10,
                stem_anchor=True,
                halo_clip=True,
            )
            by_cp = {g.code_point: (g, data) for g, data in sd.all_glyphs}
            for ch in ("n", "m", "e"):
                glyph, packed = by_cp[ord(ch)]
                self.assertGreater(glyph.width, 0)
                self.assertGreater(glyph.height, 0)
                self.assertEqual(len(packed), (glyph.width * glyph.height + 1) // 2)

    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for italic stem-anchor tests")
    def test_italic_and_oblique_skip_x_snap_while_preserving_y_overshoot_and_halo_clip(self) -> None:
        from fontTools.ttLib import TTFont

        with tempfile.TemporaryDirectory() as tmp:
            base_ttf = Path(tmp) / "Base.ttf"
            _build_synthetic_4bit_ttf(base_ttf)

            ital_ttf = Path(tmp) / "Test-Italic.ttf"
            tt_i = TTFont(str(base_ttf))
            tt_i["head"].macStyle |= 0x02
            tt_i["OS/2"].fsSelection = (tt_i["OS/2"].fsSelection & ~0x40) | 0x01
            tt_i["post"].italicAngle = -12.0
            tt_i.save(str(ital_ttf))
            tt_i.close()

            oblique_ttf = Path(tmp) / "Test-ObliquePostOnly.ttf"
            tt_o = TTFont(str(base_ttf))
            tt_o["head"].macStyle = 0
            tt_o["OS/2"].fsSelection = 0x40  # REGULAR flag only -> FT_STYLE_FLAG_ITALIC == 0
            tt_o["post"].italicAngle = -10.0
            tt_o.save(str(oblique_ttf))
            tt_o.close()

            pt_size = 12
            ppem = pt_size * 150.0 / 72.0
            outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT

            for font_path, expect_style_flag in ((ital_ttf, True), (oblique_ttf, False)):
                face = freetype.Face(str(font_path))
                face.set_char_size(pt_size << 6, pt_size << 6, 150, 150)
                has_ft_flag = bool(face.style_flags & freetype.FT_STYLE_FLAGS["FT_STYLE_FLAG_ITALIC"])
                self.assertEqual(has_ft_flag, expect_style_flag)
                self.assertTrue(_is_italic_face(face), f"{font_path.name} must be detected as italic/oblique")

                zones = _probe_stem_anchor_zones(face, outline_flags)
                for ch in ("n", "m", "e"):
                    gi = face.get_char_index(ord(ch))
                    face.load_glyph(gi, outline_flags)
                    ol = face.glyph.outline
                    raw = ol._FT_Outline
                    before_pts = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]

                    # Pass snap_x=True explicitly: _is_italic_face(face) must still skip X-axis stem snapping!
                    apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
                    after_pts = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]

                    self.assertEqual(
                        [p[0] for p in after_pts],
                        [p[0] for p in before_pts],
                        f"Italic/oblique {font_path.name} glyph {ch!r} must NOT snap or warp X coordinates",
                    )
                    if ch in ("n", "e"):
                        self.assertGreater(before_pts[2][1], zones["x_flat"])
                    self.assertEqual(after_pts[2][1], zones["x_flat"])
                    if ch == "e":
                        self.assertLess(before_pts[7][1], zones["base_flat"])
                        self.assertEqual(after_pts[7][1], zones["base_flat"])

                sd = rasterize_font_style(
                    str(font_path),
                    pt_size,
                    [(ord("e"), ord("n"))],
                    style_id=0,
                    four_bit=True,
                    gamma=1.10,
                    stem_anchor=True,
                    halo_clip=True,
                )
                by_cp = {g.code_point: (g, data) for g, data in sd.all_glyphs}
                self.assertIn(ord("n"), by_cp)
                self.assertIn(ord("e"), by_cp)

    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for bold/thin stem-anchor tests")
    def test_bold_counter_gap_protection_and_thin_stem_width_preservation(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            ttf_path = Path(tmp) / "BoldAndThinTest.ttf"
            upm = 1000
            cap_h = 700
            x_h = 500
            glyph_order = [".notdef", "space", "bar"] + [f"g_{c}" for c in "BEHTZwxyznml"]
            cmap = {ord(" "): "space", ord("|"): "bar", **{ord(c): f"g_{c}" for c in "BEHTZwxyznml"}}
            glyf = {}
            hmtx: dict[str, tuple[int, int]] = {}

            def make_glyph(name: str, adv: int, draw_fn) -> None:
                pen = TTGlyphPen(None)
                draw_fn(pen)
                g = pen.glyph()
                glyf[name] = g
                lsb = min((pt[0] for pt in g.coordinates), default=0)
                hmtx[name] = (adv, lsb)

            make_glyph(".notdef", 500, lambda p: _add_cw_rect(p, 50, 0, 450, cap_h))
            make_glyph("space", 260, lambda p: None)
            make_glyph("bar", 240, lambda p: _add_cw_rect(p, 85, -150, 155, 800))
            for c in "BEHTZ":
                make_glyph(f"g_{c}", 560, lambda p: _add_cw_rect(p, 70, 0, 490, cap_h))
            for c in "wxyz":
                make_glyph(f"g_{c}", 480, lambda p: _add_cw_rect(p, 60, 0, 420, x_h))

            def draw_bold_n(pen: "TTGlyphPen") -> None:
                pen.moveTo((40, 0))
                pen.lineTo((40, x_h))
                pen.lineTo((490, x_h))
                pen.lineTo((490, 0))
                pen.lineTo((298, 0))
                pen.lineTo((298, 380))
                pen.lineTo((232, 380))
                pen.lineTo((232, 0))
                pen.closePath()

            def draw_bold_m(pen: "TTGlyphPen") -> None:
                s0_l, s0_r = 40, 232
                s1_l, s1_r = 298, 490
                s2_l, s2_r = 556, 748
                pen.moveTo((s0_l, 0))
                pen.lineTo((s0_l, x_h))
                pen.lineTo((s2_r, x_h))
                pen.lineTo((s2_r, 0))
                pen.lineTo((s2_l, 0))
                pen.lineTo((s2_l, 380))
                pen.lineTo((s1_r, 380))
                pen.lineTo((s1_r, 0))
                pen.lineTo((s1_l, 0))
                pen.lineTo((s1_l, 380))
                pen.lineTo((s0_r, 380))
                pen.lineTo((s0_r, 0))
                pen.closePath()

            def draw_thin_l(pen: "TTGlyphPen") -> None:
                _add_cw_rect(pen, 56, 0, 114, cap_h)

            make_glyph("g_n", 530, draw_bold_n)
            make_glyph("g_m", 788, draw_bold_m)
            make_glyph("g_l", 200, draw_thin_l)

            fb = FontBuilder(upm, isTTF=True)
            fb.setupGlyphOrder(glyph_order)
            fb.setupCharacterMap(cmap)
            fb.setupGlyf(glyf)
            fb.setupHorizontalMetrics(hmtx)
            fb.setupHorizontalHeader(ascent=800, descent=-200)
            fb.setupNameTable({"familyName": "BoldAndThinTest", "styleName": "Bold"})
            fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, sxHeight=x_h, sCapHeight=cap_h, usWeightClass=700)
            fb.setupPost()
            fb.save(str(ttf_path))

            face = freetype.Face(str(ttf_path))
            pt_size = 12
            face.set_char_size(pt_size << 6, pt_size << 6, 150, 150)
            ppem = pt_size * 150.0 / 72.0
            outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT
            zones = _probe_stem_anchor_zones(face, outline_flags)

            face.load_glyph(face.get_char_index(ord("n")), outline_flags)
            raw_n = face.glyph.outline._FT_Outline
            n_before = [(raw_n.points[i].x, raw_n.points[i].y) for i in range(face.glyph.outline.n_points)]
            orig_n_gap = n_before[4][0] - n_before[7][0]
            apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
            n_after = [(raw_n.points[i].x, raw_n.points[i].y) for i in range(face.glyph.outline.n_points)]
            new_n_gap = n_after[4][0] - n_after[7][0]
            self.assertGreaterEqual(
                new_n_gap,
                max(MIN_COUNTER_GAP_26_6, MIN_COUNTER_GAP_RATIO * orig_n_gap),
                f"Bold 'n' counter gap {new_n_gap / 64:.2f}px narrowed below floor (orig {orig_n_gap / 64:.2f}px)",
            )

            face.load_glyph(face.get_char_index(ord("m")), outline_flags)
            raw_m = face.glyph.outline._FT_Outline
            m_before = [(raw_m.points[i].x, raw_m.points[i].y) for i in range(face.glyph.outline.n_points)]
            orig_m_gap01 = m_before[8][0] - m_before[11][0]
            orig_m_gap12 = m_before[4][0] - m_before[7][0]
            apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
            m_after = [(raw_m.points[i].x, raw_m.points[i].y) for i in range(face.glyph.outline.n_points)]
            new_m_gap01 = m_after[8][0] - m_after[11][0]
            new_m_gap12 = m_after[4][0] - m_after[7][0]
            self.assertGreaterEqual(new_m_gap01, max(MIN_COUNTER_GAP_26_6, MIN_COUNTER_GAP_RATIO * orig_m_gap01))
            self.assertGreaterEqual(new_m_gap12, max(MIN_COUNTER_GAP_26_6, MIN_COUNTER_GAP_RATIO * orig_m_gap12))

            face.load_glyph(face.get_char_index(ord("l")), outline_flags)
            raw_l = face.glyph.outline._FT_Outline
            l_before = [(raw_l.points[i].x, raw_l.points[i].y) for i in range(face.glyph.outline.n_points)]
            w_before = l_before[2][0] - l_before[0][0]
            self.assertLess(w_before / 64.0, 2.3)
            apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
            l_after = [(raw_l.points[i].x, raw_l.points[i].y) for i in range(face.glyph.outline.n_points)]
            w_after = l_after[2][0] - l_after[0][0]
            self.assertEqual(w_after, w_before, "Thin stem fractional 26.6 width must be 100% preserved")
            self.assertTrue(l_after[0][0] % 64 == 0 or l_after[2][0] % 64 == 0)
            face.glyph.render(freetype.FT_RENDER_MODE_NORMAL)
            bmp = face.glyph.bitmap
            lut4 = build_quant_lut(4, gamma=1.10, halo_clip=True)
            grid = _unpack_4b(pack_bitmap_4b(bmp.buffer, bmp.width, bmp.rows, bmp.pitch, lut4, True), bmp.width, bmp.rows)
            mid_row = grid[bmp.rows // 2]
            self.assertEqual(max(mid_row), 15, f"Anchored thin stem must hit full-black nibble 15, got {mid_row}")

    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for CJK/complexity skip guard tests")
    def test_cjk_and_high_complexity_skip_guard(self) -> None:
        self.assertTrue(_is_bicameral_stem_anchor_cp(ord("a")))
        self.assertTrue(_is_bicameral_stem_anchor_cp(0x03B1))  # Greek alpha
        self.assertTrue(_is_bicameral_stem_anchor_cp(0x0416))  # Cyrillic Zhe
        self.assertTrue(_is_bicameral_stem_anchor_cp(0xFB01))  # fi ligature
        self.assertFalse(_is_bicameral_stem_anchor_cp(0x4E2D))  # CJK '中'
        self.assertFalse(_is_bicameral_stem_anchor_cp(0xAC00))  # Hangul '가'
        self.assertFalse(_is_bicameral_stem_anchor_cp(0x3042))  # Hiragana 'あ'

        with tempfile.TemporaryDirectory() as tmp:
            ttf_path = Path(tmp) / "ComplexSkipTest.ttf"
            upm = 1000
            cap_h = 700
            x_h = 500
            glyph_order = [".notdef", "space", "bar", "g_many_contours", "g_many_points", "g_cjk"] + [
                f"g_{c}" for c in "BEHTZwxyz"
            ]
            cmap = {
                ord(" "): "space",
                ord("|"): "bar",
                ord("g"): "g_many_contours",
                ord("q"): "g_many_points",
                0x4E2D: "g_cjk",
                **{ord(c): f"g_{c}" for c in "BEHTZwxyz"},
            }
            glyf = {}
            hmtx: dict[str, tuple[int, int]] = {}

            def make_glyph(name: str, adv: int, draw_fn) -> None:
                pen = TTGlyphPen(None)
                draw_fn(pen)
                g = pen.glyph()
                glyf[name] = g
                lsb = min((pt[0] for pt in g.coordinates), default=0)
                hmtx[name] = (adv, lsb)

            make_glyph(".notdef", 500, lambda p: _add_cw_rect(p, 50, 0, 450, cap_h))
            make_glyph("space", 260, lambda p: None)
            make_glyph("bar", 240, lambda p: _add_cw_rect(p, 85, -150, 155, 800))
            for c in "BEHTZ":
                make_glyph(f"g_{c}", 560, lambda p: _add_cw_rect(p, 70, 0, 490, cap_h))
            for c in "wxyz":
                make_glyph(f"g_{c}", 480, lambda p: _add_cw_rect(p, 60, 0, 420, x_h))

            def draw_many_contours(pen: "TTGlyphPen") -> None:
                for idx in range(MAX_STEM_ANCHOR_CONTOURS + 2):
                    x0 = 30 + idx * 62
                    _add_cw_rect(pen, x0, 0, x0 + 44, x_h + 14)

            def draw_many_points(pen: "TTGlyphPen") -> None:
                pen.moveTo((68, 0))
                pen.lineTo((68, x_h + 14))
                steps = MAX_STEM_ANCHOR_POINTS + 10
                for k in range(steps):
                    yk = x_h + 14 - (k * 2)
                    xk = 154 + (6 if (k & 1) else 0)
                    pen.lineTo((xk, yk))
                pen.lineTo((154, 0))
                pen.closePath()

            def draw_cjk(pen: "TTGlyphPen") -> None:
                _add_cw_rect(pen, 68, 0, 154, x_h + 14)
                _add_cw_rect(pen, 412, -80, 502, 760)

            make_glyph("g_many_contours", 960, draw_many_contours)
            make_glyph("g_many_points", 500, draw_many_points)
            make_glyph("g_cjk", 1000, draw_cjk)

            fb = FontBuilder(upm, isTTF=True)
            fb.setupGlyphOrder(glyph_order)
            fb.setupCharacterMap(cmap)
            fb.setupGlyf(glyf)
            fb.setupHorizontalMetrics(hmtx)
            fb.setupHorizontalHeader(ascent=800, descent=-200)
            fb.setupNameTable({"familyName": "ComplexSkipTest", "styleName": "Regular"})
            fb.setupOS2(sTypoAscender=800, sTypoDescender=-200, sxHeight=x_h, sCapHeight=cap_h)
            fb.setupPost()
            fb.save(str(ttf_path))

            face = freetype.Face(str(ttf_path))
            pt_size = 12
            face.set_char_size(pt_size << 6, pt_size << 6, 150, 150)
            ppem = pt_size * 150.0 / 72.0
            outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT
            zones = _probe_stem_anchor_zones(face, outline_flags)

            for ch in ("g", "q"):
                face.load_glyph(face.get_char_index(ord(ch)), outline_flags)
                ol = face.glyph.outline
                raw = ol._FT_Outline
                before = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]
                apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
                after = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]
                self.assertEqual(after, before, f"High-complexity glyph {ch!r} must be skipped in O(1)")

            sd = rasterize_font_style(
                str(ttf_path),
                pt_size,
                [(0x4E2D, 0x4E2D)],
                style_id=0,
                four_bit=True,
                gamma=1.10,
                stem_anchor=True,
                halo_clip=True,
            )
            self.assertEqual(len(sd.all_glyphs), 1)
            self.assertGreater(sd.all_glyphs[0][0].width, 0)

    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for comb stem-search bound tests")
    def test_unequal_pitch_36_stem_comb_glyph_bounds_search_and_preserves_counters(self) -> None:
        # At upm=1600 and 12pt @ 150dpi (ppem=25.0), 1 font unit == 1.0 unit in 26.6 fixed-point.
        # Construct:
        #   - 'w' (36-tooth comb, 1 contour, 144 points <= 200): stem 0 at x=64..160 (dc=0),
        #     stem 1 at x=480..576 (+416 = 6.5px), stems 2..35 spaced by +384 (6.0px) so every
        #     stem k>=1 sits at an exact .5px offset from stem 0 with unequal pitch (416 vs 384)
        #     and wide counters (>= 288 >= 256). Without MAX_CHAIN_SEARCH_STEPS this branches 2^35.
        #   - 'm' (6-tooth unequal-pitch comb, 1 contour, 24 points): completes within 512 steps
        #     with bit-identical coordinates vs an unbounded cap.
        with tempfile.TemporaryDirectory() as tmp:
            ttf_path = Path(tmp) / "CombBoundTest.ttf"
            upm = 1600
            cap_h = 1120
            x_h = 832
            glyph_order = [".notdef", "space", "g_comb36", "g_comb6"] + [f"g_{c}" for c in "BEHTZwxyz"]
            cmap = {
                ord(" "): "space",
                0x0175: "g_comb36",  # 'ŵ' (bicameral Latin Extended-A)
                0x0177: "g_comb6",   # 'ŷ'
                **{ord(c): f"g_{c}" for c in "BEHTZwxyz"},
            }
            glyf = {}
            hmtx: dict[str, tuple[int, int]] = {}

            def make_glyph(name: str, adv: int, draw_fn) -> None:
                pen = TTGlyphPen(None)
                draw_fn(pen)
                g = pen.glyph()
                glyf[name] = g
                lsb = min((pt[0] for pt in g.coordinates), default=0)
                hmtx[name] = (adv, lsb)

            def make_comb_drawer(lefts: list[int], stem_w: int, y_top: int, y_slot: int):
                def _draw(pen: "TTGlyphPen") -> None:
                    # Single CW contour tracing top bar + N hanging vertical teeth
                    pen.moveTo((lefts[0], 0))
                    pen.lineTo((lefts[0], y_top))
                    pen.lineTo((lefts[-1] + stem_w, y_top))
                    pen.lineTo((lefts[-1] + stem_w, 0))
                    for idx in range(len(lefts) - 1, 0, -1):
                        pen.lineTo((lefts[idx], 0))
                        pen.lineTo((lefts[idx], y_slot))
                        pen.lineTo((lefts[idx - 1] + stem_w, y_slot))
                        pen.lineTo((lefts[idx - 1] + stem_w, 0))
                    pen.closePath()

                return _draw

            make_glyph(".notdef", 800, lambda p: _add_cw_rect(p, 64, 0, 704, cap_h))
            make_glyph("space", 400, lambda p: None)
            for c in "BEHTZ":
                make_glyph(f"g_{c}", 896, lambda p: _add_cw_rect(p, 128, 0, 768, cap_h))
            for c in "wxyz":
                make_glyph(f"g_{c}", 768, lambda p: _add_cw_rect(p, 128, 0, 640, x_h))

            stem_w = 96
            lefts_36 = [64] + [480 + (k - 1) * 384 for k in range(1, 36)]
            lefts_6 = [64] + [480 + (k - 1) * 384 for k in range(1, 6)]
            make_glyph("g_comb36", lefts_36[-1] + stem_w + 64, make_comb_drawer(lefts_36, stem_w, x_h, 640))
            make_glyph("g_comb6", lefts_6[-1] + stem_w + 64, make_comb_drawer(lefts_6, stem_w, x_h, 640))

            fb = FontBuilder(upm, isTTF=True)
            fb.setupGlyphOrder(glyph_order)
            fb.setupCharacterMap(cmap)
            fb.setupGlyf(glyf)
            fb.setupHorizontalMetrics(hmtx)
            fb.setupHorizontalHeader(ascent=1280, descent=-320)
            fb.setupNameTable({"familyName": "CombBoundTest", "styleName": "Regular"})
            fb.setupOS2(sTypoAscender=1280, sTypoDescender=-320, sxHeight=x_h, sCapHeight=cap_h)
            fb.setupPost()
            fb.save(str(ttf_path))

            self.assertEqual(MAX_CHAIN_SEARCH_STEPS, 512)
            face = freetype.Face(str(ttf_path))
            pt_size = 12
            face.set_char_size(pt_size << 6, pt_size << 6, 150, 150)
            ppem = pt_size * 150.0 / 72.0
            outline_flags = freetype.FT_LOAD_DEFAULT | freetype.FT_LOAD_TARGET_LIGHT
            zones = _probe_stem_anchor_zones(face, outline_flags)

            # (a) 36-stem unequal-pitch comb glyph completes rapidly (< 200 ms) and preserves counters & widths
            gi_36 = face.get_char_index(0x0175)
            face.load_glyph(gi_36, outline_flags)
            ol = face.glyph.outline
            self.assertEqual(ol.n_contours, 1)
            self.assertEqual(ol.n_points, 144)
            self.assertLessEqual(ol.n_contours, MAX_STEM_ANCHOR_CONTOURS)
            self.assertLessEqual(ol.n_points, MAX_STEM_ANCHOR_POINTS)
            raw = ol._FT_Outline
            before_36 = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]

            t0 = time.perf_counter()
            apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
            elapsed_ms = (time.perf_counter() - t0) * 1000.0
            self.assertLess(elapsed_ms, 200.0, f"36-stem comb took {elapsed_ms:.2f} ms (expected < 200 ms)")

            after_36 = [(raw.points[i].x, raw.points[i].y) for i in range(ol.n_points)]
            # Extract anchored (left_x, right_x) for each of the 36 teeth from bottom points (y == 0)
            bot_xs_before = sorted(p[0] for p in before_36 if p[1] == 0)
            bot_xs_after = sorted(p[0] for p in after_36 if p[1] == 0)
            self.assertEqual(len(bot_xs_before), 72)
            self.assertEqual(len(bot_xs_after), 72)
            stems_before = [(bot_xs_before[2 * k], bot_xs_before[2 * k + 1]) for k in range(36)]
            stems_after = [(bot_xs_after[2 * k], bot_xs_after[2 * k + 1]) for k in range(36)]
            for k in range(36):
                self.assertEqual(stems_after[k][1] - stems_after[k][0], stems_before[k][1] - stems_before[k][0])
                self.assertTrue(stems_after[k][0] % 64 == 0 or stems_after[k][1] % 64 == 0)
            for k in range(1, 36):
                orig_gap = stems_before[k][0] - stems_before[k - 1][1]
                new_gap = stems_after[k][0] - stems_after[k - 1][1]
                self.assertGreaterEqual(new_gap, max(MIN_COUNTER_GAP_26_6, MIN_COUNTER_GAP_RATIO * orig_gap))

            # (b) Normal 6-stem unequal-pitch glyph produces bit-identical output vs unbounded step budget
            gi_6 = face.get_char_index(0x0177)
            face.load_glyph(gi_6, outline_flags)
            apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
            raw_6 = face.glyph.outline._FT_Outline
            bounded_6 = [(raw_6.points[i].x, raw_6.points[i].y) for i in range(face.glyph.outline.n_points)]

            orig_cap = fcs.MAX_CHAIN_SEARCH_STEPS
            try:
                fcs.MAX_CHAIN_SEARCH_STEPS = 1_000_000
                face.load_glyph(gi_6, outline_flags)
                apply_stem_anchor_pass(face, ppem, zones, snap_x=True)
                raw_6_unbounded = face.glyph.outline._FT_Outline
                unbounded_6 = [
                    (raw_6_unbounded.points[i].x, raw_6_unbounded.points[i].y)
                    for i in range(face.glyph.outline.n_points)
                ]
            finally:
                fcs.MAX_CHAIN_SEARCH_STEPS = orig_cap
            self.assertEqual(bounded_6, unbounded_6)


class TestBuildSdFontsCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        spec = importlib.util.spec_from_file_location("build_sd_fonts", SCRIPTS_DIR / "build-sd-fonts.py")
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        cls.bsf = mod

    def test_default_2bit_command_is_100_percent_backward_compatible(self) -> None:
        parser = self.bsf.build_parser()
        args = parser.parse_args([])
        self.assertFalse(args.four_bit)
        self.assertFalse(args.two_bit)
        self.assertIsNone(args.gamma)
        self.assertIsNone(args.stem_anchor)
        self.assertIsNone(args.halo_clip)

        fam = {
            "name": "NotoSerif",
            "intervals": "latin-ext",
            "sizes": [12, 14, 16, 18],
            "styles": {"regular": {}, "italic": {}},
        }
        resolved = {"regular": Path("/tmp/NotoSerif-Regular.ttf"), "italic": Path("/tmp/NotoSerif-Italic.ttf")}
        cmd = self.bsf.build_fontconvert_cmd(fam, resolved, Path("/tmp/out/NotoSerif"))
        for flag in ("--4bit", "--gamma", "--stem-anchor", "--halo-clip"):
            self.assertNotIn(flag, cmd)
        self.assertIn("--sizes", cmd)
        self.assertEqual(cmd[cmd.index("--sizes") + 1], "12,14,16,18")

    def test_4bit_cli_and_per_family_yaml_overrides(self) -> None:
        parser = self.bsf.build_parser()
        args = parser.parse_args(["--4bit", "--families", "Literata,SourceSerif4", "--sizes", "14,16"])
        self.assertTrue(args.four_bit)
        self.assertEqual(args.only, "Literata,SourceSerif4")
        self.assertEqual(args.sizes, "14,16")

        base_fam = {
            "name": "Literata",
            "intervals": "latin-ext,greek,cyrillic",
            "sizes": [12, 14, 16, 18],
            "styles": {"regular": {}, "bold": {}, "italic": {}, "bolditalic": {}},
        }
        resolved = {
            "regular": Path("/tmp/Literata-Regular.ttf"),
            "bold": Path("/tmp/Literata-Bold.ttf"),
            "italic": Path("/tmp/Literata-Italic.ttf"),
            "bolditalic": Path("/tmp/Literata-BoldItalic.ttf"),
        }
        cmd_4b = self.bsf.build_fontconvert_cmd(
            base_fam,
            resolved,
            Path("/tmp/out/Literata"),
            four_bit=True,
            sizes_override=[14, 16],
        )
        self.assertIn("--4bit", cmd_4b)
        self.assertIn("--stem-anchor", cmd_4b)
        self.assertIn("--halo-clip", cmd_4b)
        self.assertIn("--gamma", cmd_4b)
        self.assertAlmostEqual(float(cmd_4b[cmd_4b.index("--gamma") + 1]), 1.10)
        self.assertEqual(cmd_4b[cmd_4b.index("--sizes") + 1], "14,16")

        # Per-family YAML overrides apply when CLI leaves gamma/stem_anchor/halo_clip unspecified
        yaml_override_fam = {
            **base_fam,
            "gamma": 1.15,
            "stem_anchor": False,
            "halo_clip": True,
        }
        cmd_yaml = self.bsf.build_fontconvert_cmd(
            yaml_override_fam,
            resolved,
            Path("/tmp/out/Literata"),
            four_bit=True,
        )
        self.assertIn("--4bit", cmd_yaml)
        self.assertNotIn("--stem-anchor", cmd_yaml)
        self.assertIn("--halo-clip", cmd_yaml)
        self.assertAlmostEqual(float(cmd_yaml[cmd_yaml.index("--gamma") + 1]), 1.15)

        # Explicit CLI flags override per-family YAML settings
        cmd_cli_override = self.bsf.build_fontconvert_cmd(
            yaml_override_fam,
            resolved,
            Path("/tmp/out/Literata"),
            four_bit=True,
            gamma=1.05,
            stem_anchor=True,
            halo_clip=False,
        )
        self.assertIn("--4bit", cmd_cli_override)
        self.assertIn("--stem-anchor", cmd_cli_override)
        self.assertNotIn("--halo-clip", cmd_cli_override)
        self.assertAlmostEqual(float(cmd_cli_override[cmd_cli_override.index("--gamma") + 1]), 1.05)

    @unittest.skipUnless(HAS_FONT_DEPS, "freetype-py and fontTools required for 4-bit shaping integration test")
    def test_4bit_stem_anchor_with_shaping_packs_4bit_glyphs_and_cpsh(self) -> None:
        import struct
        from unittest import mock
        from fontTools.ttLib import TTFont

        with tempfile.TemporaryDirectory() as tmp:
            ttf_path = Path(tmp) / "IndicSynthetic.ttf"
            _build_synthetic_4bit_ttf(ttf_path)
            # Map Devanagari KA (U+0915) onto an existing non-empty glyph so scripts_in_intervals triggers.
            tt = TTFont(str(ttf_path))
            for subtable in tt["cmap"].tables:
                if subtable.isUnicode():
                    subtable.cmap[0x0915] = "g_n"
            tt.save(str(ttf_path))
            tt.close()
            ttf_bytes = ttf_path.read_bytes()
            fake_layout = b"FAKE_OT_LAYOUT_TABLES_0123456789"
            glyph_count = 4
            out_cpfont = Path(tmp) / "IndicSynthetic_12.cpfont"

            with (
                mock.patch.object(fcs.shaping_blob, "font_supports_script", return_value=True),
                mock.patch.object(
                    fcs.shaping_blob,
                    "build",
                    return_value=(ttf_bytes, fake_layout, glyph_count),
                ),
            ):
                # Regression check: stem_anchor=True must not raise UnboundLocalError on load_flags,
                # and shaped glyphs (GLYPH_TOKEN_BASE + gid) must pack as 4bpp matching the file header.
                intervals = [(0x0020, 0x007E), (0x0915, 0x0915)]
                sd = rasterize_font_style(
                    str(ttf_path),
                    12,
                    intervals,
                    style_id=0,
                    four_bit=True,
                    gamma=1.10,
                    stem_anchor=True,
                    halo_clip=True,
                    shaping=True,
                )
                by_cp = {g.code_point: (g, data) for g, data in sd.all_glyphs}
                base = fcs.shaping_blob.GLYPH_TOKEN_BASE
                for gid in range(glyph_count):
                    self.assertIn(base + gid, by_cp)
                    glyph, packed = by_cp[base + gid]
                    expected_len = (glyph.width * glyph.height + 1) // 2
                    self.assertEqual(len(packed), expected_len)
                    self.assertEqual(glyph.data_length, expected_len)
                self.assertTrue(sd.shaping_section.startswith(b"CPSH"))

                fcs.generate_cpfont_multistyle(
                    {0: str(ttf_path)},
                    12,
                    intervals,
                    str(out_cpfont),
                    four_bit=True,
                    gamma=1.10,
                    stem_anchor=True,
                    halo_clip=True,
                    shaping=True,
                )

            raw = out_cpfont.read_bytes()
            magic, version, flags, style_count = struct.unpack_from("<8sHHB", raw, 0)
            self.assertEqual(magic, b"CPFONT\x00\x00")
            self.assertEqual(version, fcs.CPFONT_VERSION)
            self.assertEqual(flags, fcs.CPFONT_FLAG_4BIT)
            self.assertEqual(style_count, 1)
            shaping_offset = struct.unpack_from("<I", raw, 32 + 28)[0]
            self.assertGreater(shaping_offset, 0)
            self.assertEqual(raw[shaping_offset : shaping_offset + 4], b"CPSH")
            sh_ver = struct.unpack_from("<H", raw, shaping_offset + 4)[0]
            sh_len = struct.unpack_from("<I", raw, shaping_offset + 12)[0]
            self.assertEqual(sh_ver, fcs.shaping_blob.SECTION_VERSION)
            self.assertEqual(sh_len, len(fake_layout))
            self.assertEqual(raw[shaping_offset + 20 : shaping_offset + 20 + sh_len], fake_layout)


def _candidate_venv_pythons() -> list[Path]:
    candidates = [ROOT / ".venv" / "bin" / "python"]
    git_marker = ROOT / ".git"
    if git_marker.is_file():
        try:
            line = git_marker.read_text(encoding="utf-8").strip()
            if line.startswith("gitdir:"):
                git_dir = (ROOT / line.split(":", 1)[1].strip()).resolve()
                commondir = git_dir / "commondir"
                if commondir.is_file():
                    common = (git_dir / commondir.read_text(encoding="utf-8").strip()).resolve()
                    candidates.append(common.parent / ".venv" / "bin" / "python")
        except OSError:
            pass
    return candidates


def main() -> None:
    if not HAS_FONT_DEPS and os.environ.get("_FONTCONVERT_VENV_REEXEC") != "1":
        for venv_py in _candidate_venv_pythons():
            if venv_py.is_file() and Path(sys.prefix).resolve() != venv_py.parent.parent.resolve():
                probe = subprocess.run(
                    [str(venv_py), "-c", "import freetype, fontTools"],
                    stdout=subprocess.DEVNULL,
                    stderr=subprocess.DEVNULL,
                )
                if probe.returncode == 0:
                    env = dict(os.environ)
                    env["_FONTCONVERT_VENV_REEXEC"] = "1"
                    res = subprocess.run([str(venv_py), str(Path(__file__).resolve()), *sys.argv[1:]], env=env)
                    sys.exit(res.returncode)
    unittest.main()


if __name__ == "__main__":
    main()
