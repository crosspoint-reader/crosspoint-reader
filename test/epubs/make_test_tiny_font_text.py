#!/usr/bin/env python3
"""Build test_tiny_font_text.epub -- text hidden by a font size nobody can read.

test_invisible_text.epub covers text made invisible by its colour or opacity. This one covers the
other way a watermarking service hides a run in the prose: a size too small to see. Polish
publishers stamp books with spans like

    <span style="color:white; font-size:1px;">===LxkoGigb...</span>

The parser turned 1px into a 0.0625 multiplier and then raised it to the MIN_WORD_SIZE_PCT floor
(30 %), so the watermark rendered as a run of small gibberish (crosspoint-reader PR #2326).

What each chapter pins:
  1. the watermark shape exactly        -- inline 1px span mid-sentence, and the same via a class
  2. the threshold, a tenth of the body -- 1.5px, 1pt, 0.09em and 9% go; 10%, 0.1em, 2px and
                                           xx-small stay (raised to the 30 % floor, not hidden)
  3. legitimately small text            -- sup/sub, a 0.6em footnote marker, <small>, small caps
                                           and <small><sup> must all survive
  4. block and nesting                  -- a 1px paragraph, a 1px span holding a child span, and
                                           a 1px span holding an IMAGE: the image stays
  5. the root is a baseline             -- <body style="font-size:1px"> sizes the chapter, it does
                                           not hide it
  6. font-size: 0                       -- the zero-size container idiom: its children re-declare
                                           a size and keep their text
  7. a visible control chapter          -- nothing here may disappear

Every ghost carries a unique UPPERCASE marker so a golden that leaks one is a regression rather
than a pass, and every chapter carries visible text so a golden that loses the visible half is
too. Goldens only, per corpus convention; the markers make a wrong golden reviewable.

Regenerate with:  python test/epubs/make_test_tiny_font_text.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_tiny_font_text.epub")

CSS = """.wm { color: #ffffff; font-size: 1px; }
.noteref { font-size: 0.6em; vertical-align: super; }
.sc { font-variant: small-caps; }
"""

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body{body_attrs}>
    <h1>{title}</h1>
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


# (title, body, extra attributes on <body>)
CHAPTERS = [
    (
        "Watermark",
        '    <p>VISIBLEBEFORE the sentence goes on <span style="color:white; font-size:1px;">'
        "===INLINEGHOST(LxkoGigb)===</span> and VISIBLEAFTER it ends.</p>\n"
        '    <p>VISIBLETWO a class rule <span class="wm">CLASSGHOST</span> hides the same way.</p>',
        "",
    ),
    (
        "Threshold",
        '    <p>VISIBLETHREE <span style="font-size:1.5px">PXGHOST</span>'
        ' <span style="font-size:1pt">PTGHOST</span>'
        ' <span style="font-size:0.09em">EMGHOST</span>'
        ' <span style="font-size:9%">PCTGHOST</span> end.</p>\n'
        '    <p>VISIBLEFOUR <span style="font-size:10%">TENPCTSTAYS</span>'
        ' <span style="font-size:0.1em">TENTHEMSTAYS</span>'
        ' <span style="font-size:2px">TWOPXSTAYS</span>'
        ' <span style="font-size:xx-small">XXSMALLSTAYS</span> end.</p>',
        "",
    ),
    (
        "Small But Meant",
        "    <p>VISIBLEFIVE E = mc<sup>2</sup> and H<sub>2</sub>O"
        '<a class="noteref" href="#n1">1</a> with <small>SMALLSTAYS</small>,'
        ' <span class="sc">SmallCapsStays</span> and <small><sup>SMALLSUPSTAYS</sup></small>.</p>\n'
        '    <p id="n1">VISIBLESIX the note.</p>',
        "",
    ),
    (
        "Blocks And Nesting",
        '    <p style="font-size:1px">BLOCKGHOST a whole paragraph at one pixel.</p>\n'
        '    <p>VISIBLESEVEN <span style="font-size:1px">OUTERGHOST <span>INNERGHOST</span>'
        "</span> after.</p>\n"
        '    <p><span style="font-size:1px">IMGGHOST<img src="img/mark.png" alt=""/></span></p>\n'
        "    <p>VISIBLEEIGHT the image above survives its tiny wrapper.</p>",
        "",
    ),
    (
        "Root Baseline",
        "    <p>ROOTVISIBLE a tiny body size is a baseline, not a watermark.</p>",
        ' style="font-size:1px"',
    ),
    (
        "Zero Container",
        '    <div style="font-size:0"><span style="font-size:16px">ZEROVISIBLE</span>'
        ' <span style="font-size:16px">ZEROVISIBLETWO</span></div>\n'
        "    <p>VISIBLENINE must survive.</p>",
        "",
    ),
    (
        "Nothing Hidden Here",
        "    <p>A control chapter: every word in it must appear in the golden.</p>",
        "",
    ),
]


def main():
    manifest, spine, files = [], [], {}

    files["style.css"] = CSS
    manifest.append('<item id="css" href="style.css" media-type="text/css"/>')
    files["img/mark.png"] = png_gray(40, 60)
    manifest.append('<item id="mark" href="img/mark.png" media-type="image/png"/>')

    for i, (title, body, body_attrs) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body, body_attrs=body_attrs)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Tiny Font Text</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">tiny-font-text</dc:identifier>\n'
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

    with zipfile.ZipFile(OUT, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", container)
        z.writestr("OEBPS/content.opf", opf)
        for name, content in files.items():
            z.writestr("OEBPS/" + name, content)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
