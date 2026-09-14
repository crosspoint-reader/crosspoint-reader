#!/usr/bin/env python3
"""Build test_cover_svg_wrapper.epub.

A book whose manifest names an SVG wrapper as `cover-image` instead of naming the
picture. The wrapper is the shape found in the wild: a viewBox, one <image>, and an
xlink:href pointing at the JPEG beside it.

Run from this directory:  python3 generate_test_cover_svg_wrapper.py
Requires Pillow.
"""
import os
import zipfile

from PIL import Image, ImageDraw

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_cover_svg_wrapper.epub")
W, H = 600, 900


def cover():
    """A cover that is obvious at thumbnail size: a heavy frame, a cross through the
    corners and a block of text. Anything drawn instead of it reads as a failure."""
    im = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(im)
    d.rectangle([10, 10, W - 11, H - 11], outline=0, width=8)
    d.line([10, 10, W - 11, H - 11], fill=0, width=4)
    d.line([W - 11, 10, 10, H - 11], fill=0, width=4)
    d.rectangle([60, H // 2 - 90, W - 61, H // 2 + 90], fill=255, outline=0, width=4)
    for i, line in enumerate(("COVER", "INSIDE AN", "SVG WRAPPER")):
        d.text((W // 2, H // 2 - 55 + i * 45), line, fill=0, anchor="mm")
    return im


PAGE = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en" lang="en">
<head><meta charset="UTF-8"/><title>Cover in an SVG wrapper</title></head>
<body>
<h1>Cover in an SVG wrapper</h1>
<p>The manifest of this book names <code>cover.svg</code> as its
<code>cover-image</code>. That file is not a picture: it is a wrapper holding one
<code>&lt;image&gt;</code> whose <code>xlink:href</code> points at
<code>cover.jpg</code> beside it.</p>
<p><strong>What to look for:</strong> the framed COVER card should appear beside this
book on the home screen, and on the sleep screen after reading it. Before the fix the
book has no cover at all, because the href ends in <code>.svg</code> and cover
generation handles only JPEG and PNG.</p>
<p>The wrapper is reproduced from commercial EPUBs that ship it, down to the attribute
order and the 300-odd bytes of markup.</p>
</body>
</html>
"""

SVG = (
    "<?xml version='1.0' encoding='UTF-8'?>\n"
    f'<svg viewBox="0 0 {W} {H}" baseProfile="full" version="1.1"'
    ' xmlns:ev="http://www.w3.org/2001/xml-events"'
    ' xmlns:xlink="http://www.w3.org/1999/xlink"'
    ' xmlns="http://www.w3.org/2000/svg">\n'
    f'<image height="{H}" width="{W}" y="0" x="0" xlink:href="cover.jpg"></image></svg>\n'
)

NAV = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops" xml:lang="en">
<head><meta charset="UTF-8"/><title>Contents</title></head>
<body><nav epub:type="toc" id="toc"><h1>Contents</h1>
<ol><li><a href="xhtml/p-001.xhtml">Cover in an SVG wrapper</a></li></ol></nav></body>
</html>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" xmlns:dc="http://purl.org/dc/elements/1.1/"
 version="3.0" xml:lang="en" unique-identifier="unique-id">
<metadata>
<dc:title>Cover in an SVG wrapper</dc:title>
<dc:creator>CrossPoint test suite</dc:creator>
<dc:language>en</dc:language>
<dc:identifier id="unique-id">urn:uuid:0f7b6d2a-5c31-4a08-9d44-coversvgwrapper</dc:identifier>
<meta property="dcterms:modified">2026-09-14T00:00:00Z</meta>
</metadata>
<manifest>
<item media-type="application/xhtml+xml" id="toc" href="navigation-documents.xhtml" properties="nav"/>
<item media-type="image/svg+xml" id="cover" href="image/cover.svg" properties="cover-image" fallback="cover-jpg"/>
<item media-type="image/jpeg" id="cover-jpg" href="image/cover.jpg"/>
<item media-type="application/xhtml+xml" id="p-001" href="xhtml/p-001.xhtml"/>
</manifest>
<spine><itemref linear="yes" idref="p-001"/></spine>
</package>
"""

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="item/standard.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>
"""


def main():
    import io

    jpg = io.BytesIO()
    cover().convert("RGB").save(jpg, format="JPEG", quality=85, optimize=True)

    if os.path.exists(OUT):
        os.remove(OUT)
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", zipfile.ZIP_STORED)
        for name, data in (
            ("META-INF/container.xml", CONTAINER),
            ("item/standard.opf", OPF),
            ("item/navigation-documents.xhtml", NAV),
            ("item/xhtml/p-001.xhtml", PAGE),
            ("item/image/cover.svg", SVG),
        ):
            z.writestr(name, data, zipfile.ZIP_DEFLATED)
        z.writestr("item/image/cover.jpg", jpg.getvalue(), zipfile.ZIP_DEFLATED)
    print(f"{OUT}  {os.path.getsize(OUT) / 1024:.0f} KB")


if __name__ == "__main__":
    main()
