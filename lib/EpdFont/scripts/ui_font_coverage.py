"""Which characters the UI fonts can draw, read back from the generated font headers.

Shared by build_cjk_ui_fonts.py, which decides what goes into the CJK UI faces, and
scripts/gen_i18n.py, which refuses a translation that uses a character no UI font has. A
missing glyph draws nothing in the Inter faces, which have no U+FFFD, and a replacement box in
Noto Sans 8, which has one: either way the label is wrong.

The answer comes from the headers that are actually compiled, never from the source fonts:
fontconvert.py drops every requested codepoint the font has no glyph for, so only the
emitted interval table says what the device can draw.
"""

import re
from pathlib import Path

# The faces the three UI slots and the three FIT_* slots are bound to, at either step of the
# UI font ladder (applyUiFontScale() in src/main.cpp). A string can land in any of them, so a
# character counts as covered only when ALL of them have it.
UI_FACE_HEADERS = (
    "notosans_8_regular",
    "inter_ui_10_regular",
    "inter_ui_10_bold",
    "inter_ui_12_regular",
    "inter_ui_12_bold",
    "inter_ui_14_regular",
    "inter_ui_14_bold",
)

# The languages whose characters the Inter faces do not have, the subset faces generated for
# them, and the Noto Sans CJK region each is cut from -- the region picks the glyph FORMS, which
# differ for many shared Han characters (Japanese 直 is not the Chinese one). Keyed by the
# translation's _language_code. src/CjkUiFonts.cpp binds the same faces by Language.
CJK_UI_LANGUAGES = {
    "ZH": ("zh", "SimplifiedChinese/NotoSansCJKsc-Regular.otf"),
    "JA": ("ja", "Japanese/NotoSansCJKjp-Regular.otf"),
    "KO": ("ko", "Korean/NotoSansCJKkr-Regular.otf"),
}


# WHITE SQUARE, in every CJK UI face: FallbackFaceProvider draws it for a CJK character no face has
# (CjkUiFonts::STAND_IN), so a book title the faces do not cover shows boxes where its characters
# are, instead of the few it happens to share with a translation standing alone as if they were it.
CJK_UI_STAND_IN = 0x25A1
# IDEOGRAPHIC SPACE, also in every CJK UI face: no UI face has it, and a space without a glyph has
# no advance, so the words either side of one in a CJK title would run together.
CJK_UI_IDEOGRAPHIC_SPACE = 0x3000


def cjk_face_name(lang_code: str, points: int) -> str:
    return f"cjk_ui_{CJK_UI_LANGUAGES[lang_code][0]}_{points}"


def cjk_ui_font_sizes(scripts_dir: Path) -> list:
    """CJK_UI_FONT_SIZES from font-sizes.sh, the one list of built-in font sizes."""
    text = (scripts_dir / "font-sizes.sh").read_text(encoding="utf-8")
    match = re.search(r"^CJK_UI_FONT_SIZES=\(([^)]*)\)", text, re.M)
    if not match:
        raise ValueError("font-sizes.sh has no CJK_UI_FONT_SIZES")
    return [int(s) for s in match.group(1).split()]


_INTERVAL_ROW = re.compile(r"\{\s*(0x[0-9A-Fa-f]+),\s*(0x[0-9A-Fa-f]+),\s*0x[0-9A-Fa-f]+\s*\}")


def _interval_array(text: str, symbol: str):
    match = re.search(r"EpdUnicodeInterval " + re.escape(symbol) + r"\[\d*\] = \{(.*?)\n\};", text, re.S)
    if not match:
        return None
    return [(int(a, 16), int(b, 16)) for a, b in _INTERVAL_ROW.findall(match.group(1))]


def header_codepoints(fonts_dir: Path, face: str) -> set:
    """Every codepoint the generated face `face` (builtinFonts/<face>.h) has a glyph record for."""
    text = (fonts_dir / f"{face}.h").read_text(encoding="utf-8")
    match = re.search(r"static const EpdFontData " + re.escape(face) + r" = \{(.*?)\n\};", text, re.S)
    if not match:
        raise ValueError(f"{face}.h: no EpdFontData {face}")
    # The fifth initialiser is the interval table: bitmap, glyph, glyphPacked, bitmapOffsets,
    # intervals. dedupe_font_tables.py may have hoisted it into shared_tables.h.
    fields = [re.sub(r"//.*", "", line).strip().rstrip(",") for line in match.group(1).splitlines()]
    fields = [f for f in fields if f]
    symbol = fields[4]
    intervals = _interval_array(text, symbol)
    if intervals is None:
        intervals = _interval_array((fonts_dir / "shared_tables.h").read_text(encoding="utf-8"), symbol)
    if intervals is None:
        raise ValueError(f"{face}.h: interval table {symbol} not found")
    covered = set()
    for first, last in intervals:
        covered.update(range(first, last + 1))
    return covered


def ui_face_coverage(fonts_dir: Path) -> set:
    """Codepoints every UI face can draw by itself."""
    covered = None
    for face in UI_FACE_HEADERS:
        cps = header_codepoints(fonts_dir, face)
        covered = cps if covered is None else covered & cps
    return covered


def drawable(text: str) -> set:
    """The codepoints of `text` that need a glyph: everything but control characters. A space needs
    one too, for its advance."""
    return set(ord(ch) for ch in text if ord(ch) >= 0x20)
