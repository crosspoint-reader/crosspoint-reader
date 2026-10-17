#!/usr/bin/env python3
"""Build test_css_selectors.epub — the corpus book that pins how the chapter parser applies
descendant and child selectors (CssParser complex rules, ChapterHtmlSlimParser::cssAncestry).

    blockquote p   { margin-left: 2em; font-style: italic }
    div.poem > p   { margin-left: 4em }
    .deep p        { font-weight: bold }

  1. memo        the same <p class="x"> outside, inside and again outside a blockquote: the
                 resolved-style memo is keyed by tag|class, so a style a descendant rule could
                 reach must never be reused for the next element with that key (P1-P3)
  2. child       a child of div.poem is inset, a grandchild is not (P4, P5)
  3. descendant  any depth below .deep is bold (P7)
  4. too deep    a paragraph below the 32-entry ancestor stack resolves without ancestry, so
                 the descendant rule is not applied -- bounded, and never a stale match (P6)

Chapter 2, `inherit`: a span saying font-weight / font-style / text-decoration: inherit keeps the
bold, italic or underline it sits in (I1-I3); `inherit` used to read as normal / none. (text-align:
inherit is pinned by the CSS unit tests: the corpus profile justifies, which overrides CSS alignment.)

Regenerate with:  python test/epubs/make_test_css_selectors.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_css_selectors.epub")

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

OPF = """<?xml version="1.0" encoding="UTF-8"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="BookId">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:opf="http://www.idpf.org/2007/opf">
    <dc:title>CSS Selectors Test</dc:title>
    <dc:creator>Test Suite</dc:creator>
    <dc:identifier id="BookId">urn:uuid:test-css-selectors-001</dc:identifier>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="ncx"      href="toc.ncx"        media-type="application/x-dtbncx+xml"/>
    <item id="css"      href="style.css"      media-type="text/css"/>
    <item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>
    <item id="chapter2" href="chapter2.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine toc="ncx">
    <itemref idref="chapter1"/>
    <itemref idref="chapter2"/>
  </spine>
</package>
"""

NCX = """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE ncx PUBLIC "-//NISO//DTD ncx 2005-1//EN" "http://www.daisy.org/z3986/2005/ncx-2005-1.dtd">
<ncx version="2005-1" xmlns="http://www.daisy.org/z3986/2005/ncx/">
  <head>
    <meta name="dtb:uid" content="urn:uuid:test-css-selectors-001"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>CSS Selectors Test</text></docTitle>
  <navMap>
    <navPoint id="np1" playOrder="1">
      <navLabel><text>Contextual selectors</text></navLabel>
      <content src="chapter1.xhtml"/>
    </navPoint>
    <navPoint id="np2" playOrder="2">
      <navLabel><text>inherit</text></navLabel>
      <content src="chapter2.xhtml"/>
    </navPoint>
  </navMap>
</ncx>
"""

CSS = """body { margin: 0 }
p { margin: 0; text-indent: 0 }
blockquote { margin: 0 }
blockquote p { margin-left: 2em; font-style: italic }
div.poem > p { margin-left: 4em }
.deep p { font-weight: bold }
.inh { font-weight: inherit; font-style: inherit; text-decoration: inherit }
p.emph { font-style: italic }
"""


def chapter1():
    too_deep = "<div>" * 34 + "<p>P6 below the ancestor stack: not bold.</p>" + "</div>" * 34
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>Contextual selectors</title><link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<p class="x">P1 outside: plain.</p>
<blockquote><p class="x">P2 inside a blockquote: inset and italic.</p></blockquote>
<p class="x">P3 outside again: plain, not the blockquote style.</p>
<div class="poem"><p>P4 a child of the poem: inset 4em.</p><div><p>P5 a grandchild: plain.</p></div></div>
<div class="deep">{too_deep}</div>
<div class="deep"><div><section><p>P7 under .deep: bold.</p></section></div></div>
</body>
</html>
"""


def chapter2():
    return """<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>inherit</title><link rel="stylesheet" type="text/css" href="style.css"/></head>
<body>
<p>I1 <b>bold <span class="inh">still bold</span></b> plain.</p>
<p class="emph">I2 italic <span class="inh">still italic</span> end.</p>
<p>I3 <u>underlined <span class="inh">still underlined</span></u> plain.</p>
</body>
</html>
"""


def main():
    with zipfile.ZipFile(OUT, "w") as z:
        z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/content.opf", OPF, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/toc.ncx", NCX, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/style.css", CSS, compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/chapter1.xhtml", chapter1(), compress_type=zipfile.ZIP_DEFLATED)
        z.writestr("OEBPS/chapter2.xhtml", chapter2(), compress_type=zipfile.ZIP_DEFLATED)
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
