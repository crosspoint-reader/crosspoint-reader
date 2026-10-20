#!/usr/bin/env python3
"""Generate the CJK UI faces: each CJK translation's characters, at the CJK UI sizes.

The Inter UI faces carry Latin, Cyrillic and Vietnamese. A Chinese, Japanese or Korean UI
needs a few hundred more characters per language, out of tens of thousands -- the translation
decides which. So each CJK translation gets its own 1-bit subset face per size, holding exactly
the characters it uses that the UI faces do not, plus the box that stands in for every CJK
character it lacks (CJK_UI_STAND_IN) and the ideographic space, cut from the Noto Sans CJK region of that language (the
region picks the glyph forms, which differ for many shared Han characters). src/CjkUiFonts.cpp
attaches them behind the UI faces as fallback glyphs.

1-bit like the Inter faces, and then compressed into 2 KB DEFLATE groups (compress_ui_face.py),
which transforms the header fontconvert.py wrote and never re-renders a glyph. UI screens do not
prewarm a glyph cache, so a compressed face would decode a group per glyph on every repaint; the
active UI language's faces are therefore inflated once into the language pack slot and read from
flash like an uncompressed face (CjkUiFonts, FlashLangPartition), and only the other two languages'
glyphs -- the language picker's names, CJK book titles -- decode on demand.

Run it whenever a CJK translation changes. scripts/gen_i18n.py fails the build when a
translation uses a character no UI face has, and names this script.

Usage:  python build_cjk_ui_fonts.py
Needs:  the fontconvert.py requirements; downloads the three Noto Sans CJK OTFs (~16 MB each)
        into downloaded_fonts/ on first use.
"""

import shutil
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent
REPO_ROOT = SCRIPT_DIR.parents[2]
FONTS_DIR = SCRIPT_DIR.parent / "builtinFonts"
TRANSLATIONS_DIR = REPO_ROOT / "lib" / "I18n" / "translations"
DOWNLOAD_DIR = SCRIPT_DIR / "downloaded_fonts" / "NotoSansCJK"
NOTO_CJK_URL = "https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/"

sys.path.insert(0, str(SCRIPT_DIR))
sys.path.insert(0, str(REPO_ROOT / "scripts"))
from gen_i18n import parse_yaml_file  # noqa: E402
from ui_font_coverage import (  # noqa: E402
    CJK_UI_IDEOGRAPHIC_SPACE,
    CJK_UI_LANGUAGES,
    CJK_UI_STAND_IN,
    cjk_face_name,
    cjk_ui_font_sizes,
    drawable,
    header_codepoints,
    ui_face_coverage,
)


def source_font(region_path: str) -> Path:
    dest = DOWNLOAD_DIR / Path(region_path).name
    if not dest.exists():
        dest.parent.mkdir(parents=True, exist_ok=True)
        print(f"  Downloading {dest.name}...")
        try:
            urllib.request.urlretrieve(NOTO_CJK_URL + region_path, dest)
        except Exception:
            dest.unlink(missing_ok=True)
            raise
    return dest


def translation_characters(lang_code: str) -> set:
    """Every drawable codepoint of the translation whose _language_code is `lang_code`,
    its own language name included: the language picker draws that name in every UI language."""
    for path in sorted(TRANSLATIONS_DIR.glob("*.yaml")):
        data = parse_yaml_file(str(path))
        if data.get("_language_code", "").upper() == lang_code:
            cps = set()
            for value in data.values():
                cps |= drawable(value)
            return cps
    raise SystemExit(f"no translation with _language_code {lang_code}")


def main():
    sizes = cjk_ui_font_sizes(SCRIPT_DIR)
    ui_covered = ui_face_coverage(FONTS_DIR)
    generated = []
    for lang_code, (_, region_path) in CJK_UI_LANGUAGES.items():
        needed = (translation_characters(lang_code) | {CJK_UI_STAND_IN, CJK_UI_IDEOGRAPHIC_SPACE}) - ui_covered
        font = source_font(region_path)
        with tempfile.NamedTemporaryFile("w", encoding="utf-8", suffix=".txt", delete=False) as f:
            f.write("".join(chr(cp) for cp in sorted(needed)))
            chars_path = f.name
        try:
            for points in sizes:
                face = cjk_face_name(lang_code, points)
                out = FONTS_DIR / f"{face}.h"
                cmd = [sys.executable, str(SCRIPT_DIR / "fontconvert.py"), face, str(points), str(font),
                       "--codepoints-file", chars_path]
                with open(out, "w", encoding="utf-8", newline="\n") as header:
                    subprocess.run(cmd, check=True, stdout=header, cwd=SCRIPT_DIR)
                subprocess.run([sys.executable, str(SCRIPT_DIR / "compress_ui_face.py"), str(out)], check=True,
                               cwd=SCRIPT_DIR)
                generated.append((lang_code, face, needed))
                print(f"Generated {out.relative_to(REPO_ROOT)} ({len(needed)} characters)")
        finally:
            Path(chars_path).unlink(missing_ok=True)

    # The same deduplication convert-builtin-fonts.sh runs, proven byte-equivalent the same way:
    # the sizes of one language share their interval table.
    snapshot = Path(tempfile.mkdtemp())
    try:
        for header in FONTS_DIR.glob("*.h"):
            shutil.copy(header, snapshot)
        subprocess.run([sys.executable, str(SCRIPT_DIR / "dedupe_font_tables.py"), str(FONTS_DIR)], check=True)
        subprocess.run([sys.executable, str(SCRIPT_DIR / "verify_font_dedup.py"), str(FONTS_DIR), str(snapshot)],
                       check=True)
    finally:
        shutil.rmtree(snapshot)

    # fontconvert.py silently drops a codepoint its font has no glyph for. Nothing in a
    # translation should hit that, but if it does the label has a hole in it: say so here.
    missing = False
    for lang_code, face, needed in generated:
        absent = needed - header_codepoints(FONTS_DIR, face)
        if absent:
            missing = True
            print(f"ERROR: {face}: the source font has no glyph for "
                  + " ".join(f"U+{cp:04X}" for cp in sorted(absent)))
    if missing:
        sys.exit(1)


if __name__ == "__main__":
    main()
