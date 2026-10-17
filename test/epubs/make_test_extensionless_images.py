#!/usr/bin/env python3
"""Build test_extensionless_images.epub -- images whose href has no (or the wrong) extension.

FB2-to-EPUB converters reference their pictures as <img src="images/img_32"/>: the archive entry
has no extension at all. The parser gated every image on its href's extension, so these were laid
out as alt text and never extracted (crosspoint-reader PR #2386). The manifest's media-type is
right; only the name is bare.

What each chapter pins:
  1. the FB2 shape                  -- a PNG and a JPEG with no extension both lay out as images
                                       (cached as .img), not as their alt text
  2. a wrong extension              -- a PNG named .jpg lays out at the size its bytes say
  3. not an image                   -- a bare name holding text, and an .svg, both end as alt text
  4. size attributes                -- a bare name with width/height still takes its size from
                                       its bytes, which are what prove it is an image
  5. a table cell and a dotted dir  -- the cell path (buildCellImage) and a bare name under
                                       "img.d/", whose dot is not the file's extension
  6. a visible control chapter      -- an ordinary .png beside them is unchanged

Every alt text carries an UPPERCASE marker, so a golden that shows one where an image belongs is a
regression rather than a pass. The JPEG is copied out of test_jpeg_images.epub.

Regenerate with:  python test/epubs/make_test_extensionless_images.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "test_extensionless_images.epub")
JPEG_SOURCE = (os.path.join(HERE, "test_jpeg_images.epub"), "OEBPS/images/jpeg_format.jpg")

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
  </head>
  <body>
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


def jpeg_bytes():
    with zipfile.ZipFile(JPEG_SOURCE[0]) as z:
        return z.read(JPEG_SOURCE[1])


CHAPTERS = [
    (
        "No Extension",
        '    <p>VISIBLEONE a PNG and a JPEG named the FB2 way.</p>\n'
        '    <p><img src="images/img_32" alt="PNGNOEXT"/></p>\n'
        '    <p><img src="images/img_33" alt="JPEGNOEXT"/></p>',
    ),
    (
        "Wrong Extension",
        '    <p>VISIBLETWO a PNG named .jpg.</p>\n'
        '    <p><img src="images/really_png.jpg" alt="WRONGEXT"/></p>',
    ),
    (
        "Not An Image",
        '    <p>VISIBLETHREE neither of these can be drawn.</p>\n'
        '    <p><img src="images/notes" alt="TEXTSTAYSALT"/></p>\n'
        '    <p><img src="images/figure.svg" alt="SVGSTAYSALT"/></p>',
    ),
    (
        "Size Attributes",
        '    <p>VISIBLEFOUR the tag says 10x10, the bytes say 30x50.</p>\n'
        '    <p><img src="images/img_34" width="10" height="10" alt="ATTRNOEXT"/></p>',
    ),
    (
        "Cell And Dotted Directory",
        '    <table><tr><td>VISIBLEFIVE</td><td><img src="images/img_32" alt="CELLNOEXT"/></td></tr></table>\n'
        '    <p><img src="img.d/img_35" alt="DOTDIRNOEXT"/></p>',
    ),
    (
        "Ordinary Image",
        '    <p>VISIBLESIX an ordinary PNG with its extension.</p>\n'
        '    <p><img src="images/plain.png" alt="PLAINPNG"/></p>',
    ),
]


def main():
    manifest, spine, files = [], [], {}

    png = png_gray(40, 60)
    images = [
        ("img32", "images/img_32", png, "image/png"),
        ("img33", "images/img_33", jpeg_bytes(), "image/jpeg"),
        ("img34", "images/img_34", png_gray(30, 50), "image/png"),
        ("img35", "img.d/img_35", png, "image/png"),
        ("wrong", "images/really_png.jpg", png, "image/jpeg"),
        ("notes", "images/notes", b"These are notes, not a picture.\n" * 8, "image/png"),
        ("figure", "images/figure.svg", b'<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10"/>',
         "image/svg+xml"),
        ("plain", "images/plain.png", png, "image/png"),
    ]
    for item_id, href, data, media_type in images:
        files[href] = data
        manifest.append(f'<item id="{item_id}" href="{href}" media-type="{media_type}"/>')

    for i, (title, body) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Extensionless Images</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">extensionless-images</dc:identifier>\n'
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
