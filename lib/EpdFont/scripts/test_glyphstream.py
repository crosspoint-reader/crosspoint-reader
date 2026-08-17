import subprocess
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

from lib.EpdFont.scripts.glyphstream import (
    HAS_REF_FLAG,
    RAW_FLAG,
    GlyphBitmap,
    GlyphStreamModel,
    RangeDecoder,
    RangeEncoder,
    Reference,
    TreeModel,
    TreeNode,
    accumulate_training_counts,
    bootstrap_references,
    decode_font,
    emit_model_header,
    encode_font,
    gray_features,
    grow_tree,
    ink_features,
    load_bitmap_dump,
    load_model_header,
    pack_bitmap,
    packed_size,
    save_bitmap_dump,
    select_reference,
    train_model,
    unpack_bitmap,
)
from lib.EpdFont.scripts.validate_glyphstream_fonts import parse_font_header_text, validate_parsed_font


class RangeCoderTest(unittest.TestCase):
    def test_roundtrips_static_probabilities(self):
        probabilities = [128, 8192, 32768, 65408] * 32
        expected_bits = [(index * 7) & 1 for index in range(len(probabilities))]

        encoder = RangeEncoder()
        for probability, bit in zip(probabilities, expected_bits):
            encoder.encode_bit(probability, bit)
        payload = encoder.finish()

        decoder = RangeDecoder(payload)
        actual_bits = [decoder.decode_bit(probability) for probability in probabilities]
        self.assertEqual(expected_bits, actual_bits)

    def test_empty_payload_zero_fills_decoder_input(self):
        decoder = RangeDecoder(b"")
        self.assertEqual([1, 1, 1, 1], [decoder.decode_bit(65408) for _ in range(4)])


class BitmapPackingTest(unittest.TestCase):
    def test_two_bit_pixels_are_msb_first_and_continuous_across_rows(self):
        pixels = [0, 1, 2, 3, 1, 0, 3]
        packed = pack_bitmap(pixels, is_2bit=True)

        self.assertEqual(bytes([0x1B, 0x4C]), packed)
        self.assertEqual(pixels, unpack_bitmap(packed, width=7, height=1, is_2bit=True))
        self.assertEqual(2, packed_size(width=7, height=1, is_2bit=True))

    def test_one_bit_pixels_are_msb_first_and_continuous_across_rows(self):
        pixels = [1, 0, 1, 1, 0, 0, 1, 0, 1]
        packed = pack_bitmap(pixels, is_2bit=False)

        self.assertEqual(bytes([0xB2, 0x80]), packed)
        self.assertEqual(pixels, unpack_bitmap(packed, width=3, height=3, is_2bit=False))
        self.assertEqual(2, packed_size(width=3, height=3, is_2bit=False))

    def test_empty_bitmap_has_no_bytes(self):
        self.assertEqual(b"", pack_bitmap([], is_2bit=True))
        self.assertEqual([], unpack_bitmap(b"", width=0, height=9, is_2bit=True))
        self.assertEqual(0, packed_size(width=0, height=9, is_2bit=True))


def reference_fixture_model() -> GlyphStreamModel:
    return GlyphStreamModel(
        ink=TreeModel((TreeNode(16, -1, -2),), (128, 65408)),
        hi=TreeModel((), (32768,)),
        lo=TreeModel((), (32768,)),
    )


class GlyphStreamEncodingTest(unittest.TestCase):
    def test_small_glyph_uses_raw_fallback_with_only_raw_flag(self):
        glyphs = [GlyphBitmap(1, 1, [0], ord("A"))]

        streams, references = encode_font(glyphs, is_2bit=False, model=reference_fixture_model())

        self.assertEqual(RAW_FLAG, streams[0][0])
        self.assertEqual(Reference(), references[0])
        self.assertEqual(glyphs, decode_font(glyphs, streams, is_2bit=False, model=reference_fixture_model()))

    def test_empty_glyph_has_no_stream_bytes(self):
        glyphs = [GlyphBitmap(0, 0, [], ord(" "))]

        streams, references = encode_font(glyphs, is_2bit=False, model=reference_fixture_model())

        self.assertEqual([b""], streams)
        self.assertEqual([Reference()], references)
        self.assertEqual(glyphs, decode_font(glyphs, streams, is_2bit=False, model=reference_fixture_model()))

    def test_reference_selection_keeps_ascii_glyph_as_the_base(self):
        pixels = [int((x + y) % 2 == 0) for y in range(8) for x in range(8)]
        glyphs = [GlyphBitmap(8, 8, pixels, ord("a")), GlyphBitmap(8, 8, pixels, 0x00E1)]

        reference = select_reference(
            1,
            glyphs,
            [Reference()],
            reference_fixture_model(),
            is_2bit=False,
        )

        self.assertEqual(0, reference.base_index)
        self.assertEqual(0, reference.shift)

    def test_reference_selection_never_uses_extended_glyph_for_ascii_target(self):
        pixels = [int((x + y) % 2 == 0) for y in range(8) for x in range(8)]
        glyphs = [GlyphBitmap(8, 8, pixels, 0x00E1), GlyphBitmap(8, 8, pixels, ord("a"))]

        reference = select_reference(
            1,
            glyphs,
            [Reference()],
            reference_fixture_model(),
            is_2bit=False,
        )

        self.assertEqual(Reference(), reference)

    def test_reference_chain_depth_two_roundtrips(self):
        base_pixels = [int((x + y) % 2 == 0) for y in range(8) for x in range(8)]
        derived_pixels = base_pixels.copy()
        derived_pixels[1] = 1
        glyphs = [
            GlyphBitmap(8, 8, base_pixels, ord("a")),
            GlyphBitmap(8, 8, derived_pixels, 0x00E1),
            GlyphBitmap(8, 8, derived_pixels, 0x1EA1),
        ]

        streams, references = encode_font(glyphs, is_2bit=False, model=reference_fixture_model())
        decoded = decode_font(glyphs, streams, is_2bit=False, model=reference_fixture_model())

        self.assertEqual(0, references[1].base_index)
        self.assertEqual(1, references[1].depth)
        self.assertEqual(1, references[2].base_index)
        self.assertEqual(2, references[2].depth)
        self.assertEqual(HAS_REF_FLAG, streams[2][0] & HAS_REF_FLAG)
        self.assertEqual(bytes([1, 0]), streams[2][1:3])
        self.assertEqual(glyphs, decoded)

    def test_two_bit_stream_roundtrips_all_pixel_levels(self):
        pixels = [0, 1, 2, 3] * 16
        glyphs = [GlyphBitmap(8, 8, pixels, ord("A"))]

        streams, _ = encode_font(glyphs, is_2bit=True, model=reference_fixture_model())

        self.assertEqual(glyphs, decode_font(glyphs, streams, is_2bit=True, model=reference_fixture_model()))


class GeneratedHeaderValidationTest(unittest.TestCase):
    @staticmethod
    def fixture_header(bitmap: str = "0x40, 0x80,", second_offset: int = 2, bitmap_format: int = 1) -> str:
        return f"""
static const uint8_t fixtureBitmaps[4] = {{
    {bitmap} 0x40, 0x00,
}};
static const EpdGlyph fixtureGlyphs[] = {{
    {{ 1, 1, 16, 0, 1, 2, 0 }},
    {{ 1, 1, 16, 0, 1, 2, {second_offset} }},
}};
static const EpdUnicodeInterval fixtureIntervals[] = {{
    {{ 0x41, 0x42, 0x0 }},
}};
static const EpdFontData fixture = {{
    fixtureBitmaps, fixtureGlyphs, fixtureIntervals, 1, 1, 1, 0, false,
    nullptr, 0, nullptr, nullptr, nullptr, nullptr, 0, 0, 0, 0,
    nullptr, 0, nullptr, nullptr, nullptr, {bitmap_format},
}};
"""

    def test_valid_generated_header_decodes_against_ground_truth(self):
        parsed = parse_font_header_text(self.fixture_header())
        expected = [GlyphBitmap(1, 1, [1], 0x41), GlyphBitmap(1, 1, [0], 0x42)]

        validate_parsed_font(parsed, expected, is_2bit=False, model=reference_fixture_model())

    def test_altered_bitmap_byte_is_rejected(self):
        parsed = parse_font_header_text(self.fixture_header(bitmap="0x40, 0x00,"))
        expected = [GlyphBitmap(1, 1, [1], 0x41), GlyphBitmap(1, 1, [0], 0x42)]

        with self.assertRaisesRegex(ValueError, "pixel mismatch"):
            validate_parsed_font(parsed, expected, is_2bit=False, model=reference_fixture_model())

    def test_overlapping_stream_offsets_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "offset"):
            parse_font_header_text(self.fixture_header(second_offset=1))

    def test_non_glyphstream_bitmap_format_is_rejected(self):
        with self.assertRaisesRegex(ValueError, "bitmapFormat"):
            parse_font_header_text(self.fixture_header(bitmap_format=0))


class CppFixtureGeneratorTest(unittest.TestCase):
    @staticmethod
    def run_generator(*arguments: str) -> subprocess.CompletedProcess[str]:
        repo_root = Path(__file__).resolve().parents[3]
        return subprocess.run(
            [
                sys.executable,
                str(repo_root / "test/glyph_stream/generate_fixtures.py"),
                *arguments,
            ],
            cwd=repo_root,
            check=False,
            capture_output=True,
            text=True,
        )

    def test_committed_cpp_fixture_header_is_current(self):
        result = self.run_generator("--check")

        self.assertEqual(0, result.returncode, result.stderr)

    def test_check_detects_stale_header_without_rewriting_it(self):
        with TemporaryDirectory() as directory:
            output_path = Path(directory) / "GlyphStreamFixtures.generated.h"
            generated = self.run_generator("--output", str(output_path))
            self.assertEqual(0, generated.returncode, generated.stderr)
            stale_contents = output_path.read_text(encoding="utf-8") + "// stale\n"
            output_path.write_text(stale_contents, encoding="utf-8")

            checked = self.run_generator("--check", "--output", str(output_path))

            self.assertNotEqual(0, checked.returncode)
            self.assertIn("out of date", checked.stderr)
            self.assertEqual(stale_contents, output_path.read_text(encoding="utf-8"))


class ModelTrainingTest(unittest.TestCase):
    def test_tree_growth_selects_feature_that_predicts_outcome(self):
        samples = [
            ((0, 0), 0),
            ((0, 1), 1),
            ((1, 0), 0),
            ((1, 1), 1),
        ]

        tree = grow_tree(samples, leaf_budget=2)

        self.assertEqual(1, tree.nodes[0].feature)
        self.assertEqual(2, len(tree.probabilities))

    def test_bootstrap_references_never_select_depth_two_base(self):
        glyphs = [
            GlyphBitmap(4, 4, [0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]),
            GlyphBitmap(4, 4, [0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0]),
            GlyphBitmap(4, 4, [0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0]),
            GlyphBitmap(4, 4, [0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 0, 0, 0]),
        ]

        references = bootstrap_references(glyphs)

        self.assertEqual(0, references[1].base_index)
        self.assertEqual(1, references[2].base_index)
        self.assertEqual(2, references[2].depth)
        self.assertNotEqual(2, references[3].base_index)
        self.assertLessEqual(max(ref.depth for ref in references), 2)

    def test_extended_glyph_references_earlier_ascii_base(self):
        pixels = [0, 1, 0, 1, 1, 1, 1, 0, 1]
        glyphs = [
            GlyphBitmap(3, 3, pixels, ord("a")),
            GlyphBitmap(3, 3, pixels, 0x00E1),
        ]

        references = bootstrap_references(glyphs)

        self.assertEqual(-1, references[0].base_index)
        self.assertEqual(0, references[1].base_index)

    def test_bootstrap_never_uses_extended_glyph_for_ascii_target(self):
        pixels = [0, 1, 0, 1, 1, 1, 1, 0, 1]
        glyphs = [
            GlyphBitmap(3, 3, pixels, 0x00E1),
            GlyphBitmap(3, 3, pixels, ord("a")),
        ]

        references = bootstrap_references(glyphs)

        self.assertEqual(-1, references[1].base_index)

    def test_feature_vectors_follow_normative_bit_order(self):
        glyph = GlyphBitmap(3, 3, [0, 1, 2, 3, 2, 1, 1, 0, 3])
        base = GlyphBitmap(3, 3, [0, 0, 0, 0, 2, 0, 0, 0, 0])

        self.assertEqual(
            (1, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0),
            ink_features(glyph, x=1, y=1, base=base, shift=0),
        )
        self.assertEqual(
            (1, 1, 1, 0, 0, 1, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0),
            gray_features(glyph, x=1, y=1, base=base, shift=0),
        )

    def test_bitmap_dump_roundtrips_without_pickle_arrays(self):
        glyphs = [GlyphBitmap(2, 2, [0, 1, 2, 3], 0x41), GlyphBitmap(0, 0, [], 0x20)]
        with TemporaryDirectory() as directory:
            path = Path(directory) / "fixture.npz"
            save_bitmap_dump(path, "fixture_font", glyphs, is_2bit=True)

            font_name, actual_glyphs, is_2bit = load_bitmap_dump(path)

        self.assertEqual("fixture_font", font_name)
        self.assertEqual(glyphs, actual_glyphs)
        self.assertTrue(is_2bit)

    def test_generated_model_header_roundtrips_all_tree_fields(self):
        model = GlyphStreamModel(
            ink=TreeModel((TreeNode(3, -1, -2),), (1000, 64000)),
            hi=TreeModel((), (32768,)),
            lo=TreeModel((TreeNode(0, -1, -2),), (128, 65408)),
        )
        with TemporaryDirectory() as directory:
            path = Path(directory) / "glyphStreamModel.h"
            emit_model_header(model, path)
            actual = load_model_header(path)

        self.assertEqual(model, actual)

    def test_training_counts_layer_decisions_exactly_once(self):
        glyphs = [GlyphBitmap(2, 2, [0, 1, 2, 3], 0x41)]

        ink, hi, lo = accumulate_training_counts(glyphs, is_2bit=True)

        self.assertEqual(4, sum(n0 + n1 for n0, n1 in ink.values()))
        self.assertEqual((2, 1), tuple(map(sum, zip(*hi.values()))))
        self.assertEqual((1, 1), tuple(map(sum, zip(*lo.values()))))

    def test_training_builds_all_three_budgeted_trees(self):
        glyphs = [GlyphBitmap(2, 2, [0, 1, 2, 3], 0x41)]

        model = train_model([(glyphs, True)], leaf_budgets=(2, 2, 2))

        self.assertLessEqual(len(model.ink.probabilities), 2)
        self.assertLessEqual(len(model.hi.probabilities), 2)
        self.assertLessEqual(len(model.lo.probabilities), 2)

    def test_fontconvert_dump_mode_emits_only_npz(self):
        scripts_dir = Path(__file__).resolve().parent
        font_path = scripts_dir.parent / "builtinFonts/source/NotoSans/NotoSans-Regular.ttf"
        with TemporaryDirectory() as directory:
            dump_path = Path(directory) / "notosans_8_test.npz"
            result = subprocess.run(
                [
                    sys.executable,
                    str(scripts_dir / "fontconvert.py"),
                    "notosans_8_test",
                    "8",
                    str(font_path),
                    "--dump-bitmaps",
                    str(dump_path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            self.assertEqual("", result.stdout)
            font_name, glyphs, is_2bit = load_bitmap_dump(dump_path)

        self.assertEqual("notosans_8_test", font_name)
        self.assertGreater(len(glyphs), 100)
        self.assertFalse(is_2bit)

    def test_fontconvert_compress_emits_verified_glyphstream_font(self):
        scripts_dir = Path(__file__).resolve().parent
        font_path = scripts_dir.parent / "builtinFonts/source/NotoSans/NotoSans-Regular.ttf"
        result = subprocess.run(
            [
                sys.executable,
                str(scripts_dir / "fontconvert.py"),
                "notosans_8_test",
                "8",
                str(font_path),
                "--compress",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertEqual(0, result.returncode, result.stderr)
        self.assertIn("GlyphStream:", result.stderr)
        self.assertNotIn("static const EpdFontGroup", result.stdout)
        self.assertIn("    nullptr,\n    nullptr,\n    nullptr,\n    1,\n};", result.stdout)

    def test_training_cli_writes_parseable_model(self):
        scripts_dir = Path(__file__).resolve().parent
        with TemporaryDirectory() as directory:
            directory_path = Path(directory)
            dump_path = directory_path / "fixture.npz"
            output_path = directory_path / "glyphStreamModel.h"
            save_bitmap_dump(
                dump_path,
                "fixture",
                [GlyphBitmap(2, 2, [0, 1, 2, 3], 0x41)],
                is_2bit=True,
            )

            result = subprocess.run(
                [
                    sys.executable,
                    str(scripts_dir / "train_glyphstream_model.py"),
                    str(dump_path),
                    "--output",
                    str(output_path),
                ],
                check=False,
                capture_output=True,
                text=True,
            )

            self.assertEqual(0, result.returncode, result.stderr)
            model = load_model_header(output_path)

        self.assertGreaterEqual(len(model.ink.probabilities), 1)
        self.assertGreaterEqual(len(model.hi.probabilities), 1)
        self.assertGreaterEqual(len(model.lo.probabilities), 1)


if __name__ == "__main__":
    unittest.main()
