#!/usr/bin/env python3
"""
Generate test/epubs/test_dictionary.epub

Chapter source files live in scripts/epub_chapters/:
  cover.html, toc_notice.html  -- front matter (spine only, not in chapter browser)
  ch01_*.html ... ch16_*.html  -- chapters, loaded in filename sort order

To add or edit a chapter, modify the corresponding .html file and re-run this script.
"""

import glob, os, re, zipfile

SCRIPT_DIR   = os.path.dirname(os.path.abspath(__file__))
CHAPTERS_DIR = os.path.join(SCRIPT_DIR, "epub_chapters")
OUT          = os.path.join(SCRIPT_DIR, "..", "test", "epubs", "test_dictionary.epub")

# ---------------------------------------------------------------------------
# Load chapter content
# ---------------------------------------------------------------------------

def read_chapter(name):
    with open(os.path.join(CHAPTERS_DIR, name), encoding="utf-8") as f:
        return f.read()

def title_from_body(body):
    m = re.search(r"<h1>(.*?)</h1>", body)
    return m.group(1) if m else ""

COVER_BODY      = read_chapter("cover.html")
TOC_NOTICE_BODY = read_chapter("toc_notice.html")

CHAPTERS = []
for path in sorted(glob.glob(os.path.join(CHAPTERS_DIR, "ch*.html"))):
    body = open(path, encoding="utf-8").read()
    CHAPTERS.append((title_from_body(body), body))

# ---------------------------------------------------------------------------
# EPUB packaging
# ---------------------------------------------------------------------------

def xhtml(title, body):
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" "http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">
<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">
<head><title>{title}</title></head>
<body>
{body.strip()}
</body>
</html>
"""

def opf(chapters):
    items = "\n".join(
        f'    <item id="ch{i+1}" href="ch{i+1}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(len(chapters))
    )
    spine = "\n".join(
        f'    <itemref idref="ch{i+1}"/>'
        for i in range(len(chapters))
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" unique-identifier="uid" version="2.0">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>Dictionary Feature Tests</dc:title>
    <dc:identifier id="uid">test-dictionary-phase3</dc:identifier>
    <dc:language>en</dc:language>
    <dc:creator>CrossPoint Test Suite</dc:creator>
  </metadata>
  <manifest>
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
    <item id="cover" href="cover.xhtml" media-type="application/xhtml+xml"/>
    <item id="toc-notice" href="toc-notice.xhtml" media-type="application/xhtml+xml"/>
{items}
  </manifest>
  <spine toc="ncx">
    <itemref idref="cover"/>
    <itemref idref="toc-notice"/>
{spine}
  </spine>
  <guide>
    <reference type="text" href="ch1.xhtml" title="Begin"/>
  </guide>
</package>
"""

def ncx(chapters):
    points = "\n".join(
        f"""    <navPoint id="ch{i+1}" playOrder="{i+1}">
      <navLabel><text>{title}</text></navLabel>
      <content src="ch{i+1}.xhtml"/>
    </navPoint>"""
        for i, (title, _) in enumerate(chapters)
    )
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="test-dictionary-phase3"/>
    <meta name="dtb:depth" content="1"/>
    <meta name="dtb:totalPageCount" content="0"/>
    <meta name="dtb:maxPageNumber" content="0"/>
  </head>
  <docTitle><text>Dictionary Feature Tests</text></docTitle>
  <navMap>
{points}
  </navMap>
</ncx>
"""

CONTAINER = """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf"
              media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
"""

os.makedirs(os.path.dirname(OUT), exist_ok=True)

with zipfile.ZipFile(OUT, "w") as z:
    z.writestr(zipfile.ZipInfo("mimetype"), "application/epub+zip")
    z.writestr("META-INF/container.xml", CONTAINER, compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/content.opf", opf(CHAPTERS), compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/toc.ncx", ncx(CHAPTERS), compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/cover.xhtml", xhtml("Dictionary Feature Tests", COVER_BODY),
               compress_type=zipfile.ZIP_DEFLATED)
    z.writestr("OEBPS/toc-notice.xhtml", xhtml("Table of Contents", TOC_NOTICE_BODY),
               compress_type=zipfile.ZIP_DEFLATED)
    for i, (title, body) in enumerate(CHAPTERS):
        z.writestr(f"OEBPS/ch{i+1}.xhtml", xhtml(title, body),
                   compress_type=zipfile.ZIP_DEFLATED)

print(f"Written: {OUT}")
print(f"Front matter: cover.xhtml, toc-notice.xhtml (spine only, not in chapter browser)")
print(f"Chapters: {len(CHAPTERS)}")
for i, (t, _) in enumerate(CHAPTERS):
    print(f"  {i+1:2d}. {t}")
