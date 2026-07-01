#!/usr/bin/env python3
"""
Generate a bilingual EPUB for testing CrossPoint's Bilingual View Mode toggle.

The output EPUB marks each paragraph with `class="cp-original"` (source text) and
`class="cp-translation"` (translation), matching the marker classes consumed by
ChapterHtmlSlimParser's bilingual override path. Long-press Confirm (when bound to
Bilingual Toggle in Settings → Controls) cycles Both → Original → Translation and
re-parses the current section.

The parser is a hybrid: explicit cp-* class tokens win, then the standard
xml:lang attribute (vs the publication's dc:language) is used as a fallback.
Use --mode to exercise each path:

    python3 scripts/generate_bilingual_test_epub.py                 # cp-* (default)
    python3 scripts/generate_bilingual_test_epub.py --mode lang     # xml:lang only (bookfere-style)
    python3 scripts/generate_bilingual_test_epub.py --mode both     # cp-* + xml:lang
    python3 scripts/generate_bilingual_test_epub.py --mode none     # plain (toggle no-op)

Stdlib only — no Pillow/bs4/etc. The default output is test/epubs/bilingual-sample.epub
(or bilingual-sample-<mode>.epub when --mode is given).
"""

import argparse
import os
import sys
import uuid
import zipfile
from pathlib import Path

DEFAULT_OUTPUT = Path(__file__).parent.parent / "test" / "epubs" / "bilingual-sample.epub"

# Source language is "en" (matches dc:language below); translation is "ko".
SOURCE_LANG = "en"
TRANSLATION_LANG = "ko"

# A handful of public-domain (Project Gutenberg) paragraphs with their Korean
# glosses. Kept short so the EPUB renders fast on the X3/X4. A real translation
# pipeline would produce these from any source book.
SAMPLE_PARAGRAPHS = [
    (
        "It is a truth universally acknowledged, that a single man in possession of a good fortune, must be in want of a wife.",
        "많은 돈을 가진 미혼 남자라면 아내가 필요할 것이라는 점은, 누구나 인정하는 보편적 진리이다.",
    ),
    (
        "However little known the feelings or views of such a man may be on his first entering a neighbourhood, this truth is so well fixed in the minds of the surrounding families, that he is considered as the rightful property of some one or other of their daughters.",
        "그런 남자가 처음 어떤 동네에 이사 왔을 때 그의 감정이나 견해가 얼마나 알려져 있든 간에, 이 진리는 주변 가들의 마음속에 너무나 확고히 박혀 있어서, 그가 그 집 딸 중 한 명의 정당한 소유물로 간주된다.",
    ),
    (
        '"My dear Mr. Bennet," said his lady to him one day, "have you heard that Netherfield Park is let at last?"',
        '"친애하는 베넷 씨," 어느 날 그의 부인이 그에게 말했다, "네더필드 파크가 드디어 나갔다는 얘기 들었어요?"',
    ),
    (
        "Mr. Bennet replied that he had not.",
        "베넷 씨는 못 들었다고 대답했다.",
    ),
    (
        '"But it is," returned she; "for Mrs. Long has just been here, and she told me all about it."',
        '"하지만 그래요," 그녀가 말을 이었다, "롱 부인이 방금 왔는데, 그녀가 나에게 다 얘기해 주었어요."',
    ),
]


def build_chapter_xhtml(title: str, pairs, mode: str = "cp") -> str:
    parts = [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<!DOCTYPE html>',
        '<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">',
        '<head><meta charset="utf-8"/><title>',
        title,
        '</title></head>',
        '<body>',
    ]
    for original, translation in pairs:
        o_open, t_open = _paragraph_openers(mode)
        parts.append(f"{o_open}{original}</p>")
        parts.append(f"{t_open}{translation}</p>")
    parts.append("</body></html>")
    return "".join(parts)


def _paragraph_openers(mode: str):
    """Return (<p-opener for source>, <p-opener for translation>) for the mode."""
    if mode == "cp":
        return (f'<p class="cp-original">', f'<p class="cp-translation">')
    if mode == "lang":
        # Standard xml:lang only — bookfere/Ebook-Translator style. The parser
        # falls back to this when cp-* tokens are absent.
        return (f'<p xml:lang="{SOURCE_LANG}">', f'<p xml:lang="{TRANSLATION_LANG}">')
    if mode == "both":
        # cp-* tokens + xml:lang. cp-* wins; xml:lang adds accessibility.
        return (f'<p class="cp-original" xml:lang="{SOURCE_LANG}">',
                f'<p class="cp-translation" xml:lang="{TRANSLATION_LANG}">')
    if mode == "none":
        # Plain paragraphs — toggle should be a no-op.
        return ("<p>", "<p>")
    raise ValueError(f"unknown mode: {mode!r}")


def build_epub(output_path: Path, mode: str = "cp") -> None:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    book_uid = str(uuid.uuid4())
    title = "Bilingual Sample" if mode == "cp" else f"Bilingual Sample ({mode})"

    with zipfile.ZipFile(output_path, "w", zipfile.ZIP_DEFLATED) as z:
        # 1. mimetype (uncompressed, first entry — EPUB spec)
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)

        # 2. META-INF/container.xml
        z.writestr(
            "META-INF/container.xml",
            '<?xml version="1.0" encoding="UTF-8"?>'
            '<container version="1.0" '
            'xmlns="urn:oasis:names:tc:opendocument:xmlns:container">'
            '<rootfiles><rootfile full-path="OEBPS/content.opf" '
            'media-type="application/oebps-package+xml"/></rootfiles>'
            "</container>",
        )

        # 3. OEBPS/content.opf
        z.writestr(
            "OEBPS/content.opf",
            '<?xml version="1.0" encoding="utf-8"?>'
            '<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid" version="3.0">'
            '<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">'
            f'<dc:identifier id="bookid">urn:uuid:{book_uid}</dc:identifier>'
            f"<dc:title>{title}</dc:title>"
            "<dc:language>en</dc:language>"
            "<dc:creator>CrossPoint bilingual sample generator</dc:creator>"
            "</metadata>"
            "<manifest>"
            '<item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>'
            '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
            "</manifest>"
            '<spine toc="ncx"><itemref idref="chapter1"/></spine>'
            "</package>",
        )

        # 4. OEBPS/toc.ncx
        z.writestr(
            "OEBPS/toc.ncx",
            '<?xml version="1.0" encoding="utf-8"?>'
            '<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">'
            "<head><meta name=\"dtb:uid\" content="
            f'"urn:uuid:{book_uid}"/></head>'
            "<docTitle><text>"
            f"{title}</text></docTitle>"
            '<navMap><navPoint id="navpoint-1" playOrder="1">'
            "<navLabel><text>Chapter 1</text></navLabel>"
            '<content src="chapter1.xhtml"/></navPoint></navMap>'
            "</ncx>",
        )

        # 5. OEBPS/chapter1.xhtml — the actual content with cp-original / cp-translation markers
        z.writestr(
            "OEBPS/chapter1.xhtml",
            build_chapter_xhtml("Chapter 1", SAMPLE_PARAGRAPHS, mode=mode),
        )

    print(f"wrote {output_path} ({output_path.stat().st_size} bytes) [mode={mode}]")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("output", nargs="?", type=Path, default=None,
                    help="output .epub path (default: test/epubs/bilingual-sample[-<mode>].epub)")
    ap.add_argument("--mode", choices=["cp", "lang", "both", "none"], default="cp",
                    help="marker style: cp=cp-* classes (default), lang=xml:lang only, "
                         "both=cp-* + xml:lang, none=plain (toggle no-op)")
    args = ap.parse_args()
    output = args.output
    if output is None:
        output = DEFAULT_OUTPUT if args.mode == "cp" else DEFAULT_OUTPUT.with_name(
            f"bilingual-sample-{args.mode}.epub")
    build_epub(output, mode=args.mode)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
