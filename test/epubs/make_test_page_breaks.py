#!/usr/bin/env python3
"""Build test_page_breaks.epub -- CSS page-break-before and page-break-after.

page-break-after (and break-after) was parsed and stored but never acted on: a title page, a
poem or a chapter end marked page-break-after:always ran straight on into what followed it.
page-break-before was acted on at the start tag, when the paragraph before the element had not
been laid out yet: the page went out without it, and it moved to the new page with the element.

What each chapter pins (one chapter per spine item, pages=N in the golden):
  1. break after a paragraph   -- AFTERONE ends page 0, AFTERTWO starts page 1
  2. classes                   -- a div and an h2 that break after themselves: three pages
  3. break before              -- KEEPONE and KEEPTWO stay on page 0, BREAKBEFORE starts page 1
  4. after, then before        -- one break between them, not two
  5. nested                    -- the inner paragraph and the outer div each break: three pages
  6. an image after the break  -- the image is on page 1
  7. at the end                -- a break after the last paragraph adds no empty page

Regenerate with:  python test/epubs/make_test_page_breaks.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build_test -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_page_breaks.epub")

CSS = """p, h2 { margin: 0; text-indent: 0; }
.endpart { break-after: page; }
h2 { page-break-after: always; }
.fresh { page-break-before: always; }
"""

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body>
{body}
  </body>
</html>
"""


def png_gray(width, height):
    """A tiny 8-bit greyscale PNG (diagonal gradient) -- enough for the header read and a dither."""
    rows = []
    for y in range(height):
        row = bytes(((x * 255) // (width - 1) + y * 2) & 0xFF for x in range(width))
        rows.append(b"\x00" + row)  # filter type 0 per scanline
    raw = b"".join(rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)  # 8-bit, greyscale, no interlace
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr) + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")


CHAPTERS = [
    (
        "After A Paragraph",
        '    <p style="page-break-after:always">AFTERONE ends the page.</p>\n'
        "    <p>AFTERTWO starts the next.</p>",
    ),
    (
        "Classes",
        '    <div class="endpart"><p>PARTONE first.</p><p>PARTTWO second.</p></div>\n'
        "    <h2>TITLEHEAD</h2>\n"
        "    <p>TITLEBODY under its own page.</p>",
    ),
    (
        "Break Before",
        "    <p>KEEPONE stays.</p>\n"
        "    <p>KEEPTWO stays as well.</p>\n"
        '    <p class="fresh">BREAKBEFORE starts the page.</p>',
    ),
    (
        "After Then Before",
        '    <p style="page-break-after:always">ONEBREAK before.</p>\n'
        '    <p class="fresh">ONEBREAKNEXT after.</p>',
    ),
    (
        "Nested",
        '    <div style="page-break-after:always"><p style="page-break-after:always">NESTINNER</p>'
        "<p>NESTSIBLING</p></div>\n"
        "    <p>NESTOUTSIDE</p>",
    ),
    (
        "Image After",
        '    <p style="page-break-after:always">PICCAPTION above.</p>\n'
        '    <div><img src="img/pic.png" alt=""/></div>',
    ),
    (
        "At The End",
        "    <p>ENDONE first.</p>\n"
        '    <p style="page-break-after:always">ENDLAST and nothing after.</p>',
    ),
]


def main():
    manifest, spine, files = [], [], {}

    files["style.css"] = CSS
    manifest.append('<item id="css" href="style.css" media-type="text/css"/>')
    files["img/pic.png"] = png_gray(120, 60)
    manifest.append('<item id="pic" href="img/pic.png" media-type="image/png"/>')

    for i, (title, body) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Page Breaks</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">page-breaks</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n    " + "\n    ".join(manifest) + "\n  </manifest>\n"
        "  <spine>\n    " + "\n    ".join(spine) + "\n  </spine>\n"
        "</package>\n"
    )

    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )

    # Fixed timestamps: the archive is committed, so a rebuild must be byte-identical.
    def add(z, name, data, compress=zipfile.ZIP_DEFLATED):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = compress
        z.writestr(info, data)

    with zipfile.ZipFile(OUT, "w") as z:
        add(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        add(z, "META-INF/container.xml", container)
        add(z, "OEBPS/content.opf", opf)
        for name, content in files.items():
            add(z, "OEBPS/" + name, content)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
