#!/usr/bin/env python3
"""
Generate an EPUB fixture for ordered, unordered, nested, and over-depth list rendering.
"""

import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

BOOK_ID = "test-list-rendering-001"


def create_epub(filename, title, chapters):
    """Create an EPUB file with given chapters."""
    with zipfile.ZipFile(filename, "w", zipfile.ZIP_DEFLATED) as epub:
        epub.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)

        epub.writestr("META-INF/container.xml", """<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>""")

        manifest_items = []
        spine_items = []
        nav_points = []
        for i, (chapter_title, content) in enumerate(chapters):
            chapter_id = f"chapter{i}"
            manifest_items.append(
                f'<item id="{chapter_id}" href="{chapter_id}.xhtml" media-type="application/xhtml+xml"/>'
            )
            spine_items.append(f'<itemref idref="{chapter_id}"/>')
            nav_points.append(f"""
    <navPoint id="navPoint-{i + 1}" playOrder="{i + 1}">
      <navLabel><text>{chapter_title}</text></navLabel>
      <content src="{chapter_id}.xhtml"/>
    </navPoint>""")
            epub.writestr(f"OEBPS/{chapter_id}.xhtml", content)

        epub.writestr("OEBPS/style.css", """body {
  font-family: serif;
}
.test-note {
  font-style: italic;
}
""")
        manifest_items.append('<item id="style" href="style.css" media-type="text/css"/>')

        epub.writestr("OEBPS/content.opf", f"""<?xml version="1.0"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>{title}</dc:title>
    <dc:creator>CrossPoint Test Generator</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="bookid">{BOOK_ID}</dc:identifier>
  </metadata>
  <manifest>
    {''.join(manifest_items)}
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx">
    {''.join(spine_items)}
  </spine>
</package>""")

        epub.writestr("OEBPS/toc.ncx", f"""<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="{BOOK_ID}"/>
  </head>
  <docTitle><text>{title}</text></docTitle>
  <navMap>
    {''.join(nav_points)}
  </navMap>
</ncx>""")


def make_chapter(title, content):
    """Create XHTML chapter content."""
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{title}</h1>
  {content}
</body>
</html>"""


def main():
    print("Creating list rendering test EPUB...")

    long_tail = (
        "This deliberately long sentence should wrap to a second line so marker placement, "
        "hanging indent, and continuation alignment can be checked on the device."
    )

    chapters = [
        ("Introduction", make_chapter("List Rendering Tests", f"""
<p>This EPUB tests list rendering in CrossPoint Reader.</p>
<p>Check that markers stay on the same line as the first item text, wrapped lines align under the text, and nested levels indent visibly.</p>
<ul>
<li>Unordered list markers should render as bullets.</li>
<li>Ordered list markers should render as numeric ordinals.</li>
<li>Nested list rendering should remain correct through depth 4.</li>
<li>Depth 5 intentionally exceeds CrossPoint's tracked list depth and demonstrates overflow behavior.</li>
</ul>
<p class="test-note">Use the device renderer, not only desktop EPUB viewers, for final acceptance.</p>
""")),
        ("Unordered Lists", make_chapter("Unordered Lists", f"""
<p>Expected: every item below begins with a bullet marker.</p>
<ul>
<li>Plain unordered item. {long_tail}</li>
<li><p>Paragraph-wrapped unordered item. The bullet should not appear alone above this paragraph.</p></li>
<li><p>First paragraph in a multi-paragraph unordered item. The marker belongs only on this first paragraph.</p><p>Second paragraph in the same list item should align as continuation content, not as a new bullet.</p></li>
<li>Final unordered item after paragraph-wrapped cases.</li>
</ul>
""")),
        ("Ordered Lists", make_chapter("Ordered Lists", f"""
<p>Expected: the first list below uses 1., 2., 3., and 4. markers.</p>
<ol>
<li>First ordered item. {long_tail}</li>
<li><p>Second ordered item wrapped in a paragraph. The marker should be 2. and should stay with this text.</p></li>
<li><p>Third ordered item with two paragraphs. The marker should be 3.</p><p>This continuation paragraph should not receive marker 4.</p></li>
<li>Fourth ordered item after the multi-paragraph item.</li>
</ol>
<p>Expected: this second ordered list restarts at 1.</p>
<ol>
<li>Restarted ordered item one.</li>
<li>Restarted ordered item two.</li>
</ol>
""")),
        ("Nested Lists Depth 4", make_chapter("Nested Lists Depth 4", f"""
<p>Expected: nested lists render correctly through depth 4 with visible indentation and the correct marker type for each level.</p>
<ol>
<li>Depth 1 ordered item should show 1.
  <ul>
    <li>Depth 2 unordered item should show a bullet.
      <ol>
        <li>Depth 3 ordered item should show 1.
          <ul>
            <li>Depth 4 unordered item should show a bullet. {long_tail}</li>
            <li><p>Depth 4 paragraph-wrapped unordered item should keep its bullet with this paragraph.</p></li>
          </ul>
        </li>
        <li>Depth 3 ordered item should show 2.</li>
      </ol>
    </li>
    <li>Depth 2 unordered sibling should still show a bullet.</li>
  </ul>
</li>
<li>Depth 1 ordered sibling should show 2.</li>
</ol>
""")),
        ("Nested Lists Depth 5 Overflow", make_chapter("Nested Lists Depth 5 Overflow", f"""
<p>Expected: levels 1 through 4 use the correct marker type. Level 5 intentionally exceeds the parser's tracked max depth of 4.</p>
<p class="test-note">The depth 5 ordered item demonstrates overflow behavior: it should not crash, corrupt following list state, or detach the marker from text.</p>
<ol>
<li>Depth 1 ordered item should show 1.
  <ul>
    <li>Depth 2 unordered item should show a bullet.
      <ol>
        <li>Depth 3 ordered item should show 1.
          <ul>
            <li>Depth 4 unordered item should show a bullet.
              <ol>
                <li>Depth 5 ordered overflow item. This level exceeds the tracked list context limit of 4. {long_tail}</li>
                <li><p>Second depth 5 overflow item wrapped in a paragraph.</p></li>
              </ol>
            </li>
          </ul>
        </li>
      </ol>
    </li>
  </ul>
</li>
<li>Depth 1 ordered sibling after overflow should show 2., proving list state recovered after the over-depth list.</li>
</ol>
""")),
        ("Mixed Lists", make_chapter("Mixed Lists", f"""
<p>Expected: switching between ordered and unordered lists preserves each list's marker type and numbering.</p>
<ul>
<li>Outer unordered item before an ordered child.
  <ol>
    <li>Child ordered item one.</li>
    <li>Child ordered item two.
      <ul>
        <li>Grandchild unordered item after ordered item two.</li>
      </ul>
    </li>
  </ol>
</li>
<li>Outer unordered sibling after ordered child.</li>
</ul>
<ol>
<li>Outer ordered item before an unordered child.
  <ul>
    <li>Child unordered item.</li>
  </ul>
</li>
<li>Outer ordered sibling should show 2.</li>
</ol>
""")),
    ]

    output_file = OUTPUT_DIR / "test_list_rendering.epub"
    create_epub(output_file, "List Rendering Tests", chapters)
    print(f"Created: {output_file}")


if __name__ == "__main__":
    main()
