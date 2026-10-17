#!/usr/bin/env python3
"""Build test_cjk_block_seams.epub: CJK text on both sides of an element boundary, with a line break
in the pretty-printed markup between them (CjkBlockSeams.ALineBreakAtABlockOrListItemStaysASpace).

CSS removes a source line break only between two CJK characters inside one inline formatting
context. Here every line break sits next to an element that is not inline formatting, so each one
stays a word gap, as it always has:

  - a <ul> and an <ol>, each with an item whose text sits on its own indented line and a control
    item written on one line: the first characters of both sit the same distance after the marker
  - a <dl> with <dt> and <dd> on separate lines, after a CJK paragraph
  - a <section> holding text without a paragraph, followed by a <figure> with a <figcaption>

The parser does not lay out <dl>, <dt>, <dd>, <figure> or <figcaption> as blocks, so their text
shares a line with the text before it, with a space at each seam. Those spaces are correct, which is
why this book is separate from test_cjk_line_breaking.epub: the rule checker (CjkRulesTest's
cjkBooks()) asserts no space between two unspaced CJK characters on a line, a rule for text inside
one inline formatting context that does not apply at element boundaries.

Regenerate with:  python test/epubs/make_test_cjk_block_seams.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_cjk_block_seams.epub")

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
"""

BODY = (
    "<p>前の段落の文章。</p>\n"
    "<ul>\n  <li>\n    第一項目\n  </li>\n  <li>第二項目</li>\n</ul>\n"
    "<p>前の段落の文章。</p>\n"
    "<ol>\n  <li>\n    第三項目\n  </li>\n  <li>第四項目</li>\n</ol>\n"
    "<p>前の段落。</p>\n"
    "<dl>\n<dt>用語</dt>\n<dd>説明文</dd>\n</dl>\n"
    "<section>\n節の文章\n</section>\n"
    "<figure>\n<figcaption>図の説明</figcaption>\n</figure>"
)

CHAPTER = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="ja" lang="ja">
<head>
  <title>CJK block seams</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""


def main():
    opf = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>CJK Block Seams</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>ja</dc:language>\n"
        '    <dc:identifier id="bookid">cjk-block-seams</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        '    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>\n'
        "  </manifest>\n"
        '  <spine>\n    <itemref idref="chapter1"/>\n  </spine>\n'
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
        z.writestr("OEBPS/style.css", CSS)
        z.writestr("OEBPS/chapter1.xhtml", CHAPTER.format(body=BODY))
    print("wrote", OUT)


if __name__ == "__main__":
    main()
