#!/usr/bin/env python3
"""
Generate a test EPUB for <br> section-break rendering.

Tests that a bare <br> element between paragraphs produces a visible blank-line
gap (section separator), while a <br> inside a paragraph only produces a line
break with no extra spacing.

Also tests that <li> containing <p> renders the bullet inline with the paragraph
text, not on a separate line (GitHub issue #956).

Cases covered:
  1. Standalone <br> between paragraphs (section break — must show gap).
  2. <br class="..."> with a CSS class (calibre-style section break).
  3. Multiple consecutive <br> elements (each adds one line of spacing).
  4. Inline <br> inside a <p> (line break only — no extra gap).
  5. <br> at start of chapter (no gap before first paragraph).
  6. <br> following a heading.
  7. <li><p> with bold+italic text (bullet must be inline with text).
  8. <li><p> with nested <ul> (bullet inline, nested list indented).
  9. <li> with direct text (no <p> wrapper — baseline, already works).

Visual verification instructions are embedded as the first paragraph of each
chapter so a human tester can confirm the expected result on device.
"""

import os
import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_PATH = OUTPUT_DIR / "test_br_section_break.epub"

FILLER = (
    "Lorem ipsum dolor sit amet, consectetur adipiscing elit, sed do eiusmod "
    "tempor incididunt ut labore et dolore magna aliqua."
)

CSS = """\
body { margin: 0; padding: 0; }
p    { margin-top: 1pt; margin-bottom: 0; text-indent: 1em; text-align: justify; }
h1   { text-align: center; margin-top: 0.5em; margin-bottom: 0.5em; }
h2   { text-align: center; margin-top: 0.5em; margin-bottom: 0.5em; }
.section-br { display: block; }
.list-simple1 { margin-left: 1em; }
.list-item1 { text-indent: 0; }
.list-bullet { margin-left: 1em; }
.list-bullet2 { margin-left: 2em; }
.list { text-indent: 0; }
.list-item { text-indent: 0; }
"""

def xhtml(title, body):
    return f"""\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="styles/test.css"/>
</head>
<body>
{body}
</body>
</html>"""


# ---------------------------------------------------------------------------
# Chapter 1 — standalone <br> between paragraphs
# ---------------------------------------------------------------------------
ch1 = xhtml("Ch1: Standalone br", f"""
<h1>Ch 1: Standalone &lt;br&gt; Section Break</h1>
<p>PASS: A visible blank-line gap should appear between the two sections below.</p>
<p>{FILLER}</p>
<br/>
<p>{FILLER}</p>
<p>PASS: The gap above should be roughly one line tall (same as a blank line).</p>
""")

# ---------------------------------------------------------------------------
# Chapter 2 — <br class="..."> CSS-classed section break (calibre style)
# ---------------------------------------------------------------------------
ch2 = xhtml("Ch2: Classed br", f"""
<h1>Ch 2: &lt;br class="section-br"/&gt;</h1>
<p>PASS: A blank-line gap should appear between the two sections below, identical
to Ch 1, even though the &lt;br&gt; carries a CSS class.</p>
<p>{FILLER}</p>
<br class="section-br"/>
<p>{FILLER}</p>
""")

# ---------------------------------------------------------------------------
# Chapter 3 — multiple consecutive <br> elements
# ---------------------------------------------------------------------------
ch3 = xhtml("Ch3: Multiple br", f"""
<h1>Ch 3: Multiple Consecutive &lt;br&gt; Elements</h1>
<p>PASS: Two blank lines should appear between the sections (one per &lt;br&gt;).</p>
<p>{FILLER}</p>
<br/>
<br/>
<p>{FILLER}</p>
<p>PASS: Three blank lines should appear below.</p>
<p>{FILLER}</p>
<br/>
<br/>
<br/>
<p>{FILLER}</p>
""")

# ---------------------------------------------------------------------------
# Chapter 4 — inline <br> inside a paragraph (line break, NOT a gap)
# ---------------------------------------------------------------------------
ch4 = xhtml("Ch4: Inline br", """
<h1>Ch 4: Inline &lt;br&gt; Inside a Paragraph</h1>
<p>PASS: The two lines below should be adjacent with NO extra gap between them.
The &lt;br&gt; is inside the paragraph and must only break the line.</p>
<p>First line of the paragraph.<br/>Second line of the paragraph — directly below, no gap.</p>
<p>PASS: Above should look like two closely-spaced lines, not like two paragraphs
separated by a blank line.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 5 — <br> following a heading
# ---------------------------------------------------------------------------
ch5 = xhtml("Ch5: br after heading", f"""
<h1>Ch 5: &lt;br&gt; After a Heading</h1>
<br/>
<p>PASS: There should be a blank-line gap between the heading above and this paragraph.</p>
<p>{FILLER}</p>
<h2>Section heading</h2>
<br/>
<p>PASS: There should be a blank-line gap between the section heading and this paragraph.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 6 — <br> at very start of chapter (no spurious leading gap)
# ---------------------------------------------------------------------------
ch6 = xhtml("Ch6: br at chapter start", f"""<br/>
<h1>Ch 6: &lt;br&gt; at Chapter Start</h1>
<p>PASS: This heading should appear near the top of the page with no large blank
area above it despite the &lt;br&gt; being the very first element.</p>
<p>{FILLER}</p>
""")

# ---------------------------------------------------------------------------
# Chapter 7 — <li><p> with bold+italic (issue #956 example 1)
# ---------------------------------------------------------------------------
ch7 = xhtml("Ch7: li>p bold italic", """
<h1>Ch 7: &lt;li&gt;&lt;p&gt; with Bold+Italic</h1>
<p>PASS: Each bullet below must be on the SAME line as its text, not on a
separate line above it. The bullet and text are inline.</p>
<ul class="list-simple1">
<li><p class="list-item1"><b><i>Sketching User Experiences</i> by Bill Buxton</b>. This book examines the notion of sketching (Hint: prototypes are a kind of sketch) across a wide variety of disciplines, with eye-opening results.</p></li>
<li><p class="list-item1"><b><i>Have Paper, Will Prototype</i> by Bill Lucas</b>. This lecture is a series of case studies about how to successfully create paper prototypes of computer interfaces.</p></li>
<li><p class="list-item1"><b><i>The Kobold Guide to Board Game Design</i> by Mike Selinker</b>. The very best book there is on how to design great board games.</p></li>
</ul>
<p>PASS: All three bullets above should be inline with their respective text.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 8 — <li><p> with nested <ul> (issue #956 example 2)
# ---------------------------------------------------------------------------
ch8 = xhtml("Ch8: li>p nested ul", """
<h1>Ch 8: &lt;li&gt;&lt;p&gt; with Nested List</h1>
<p>PASS: Each bullet must be inline with its text. The nested list should be
indented further with its own bullets also inline.</p>
<ul class="list-bullet">
<li><p class="list">Problem statement: Design a flying dino game where dinosaurs race in and above the water.</p></li>
<li><p class="list">Detailed problem statements</p>
<ul class="list-bullet2">
<li><p class="list-item">We need to figure out if we can schedule all the animation time needed for the dinosaurs.</p></li>
<li><p class="list-item">We need to develop the right number of levels for this game.</p></li>
<li><p class="list-item">We need to figure out all the power-ups that will go into this game.</p></li>
</ul></li>
</ul>
<p>PASS: All bullets above should be inline with their text, including the nested ones.</p>
""")

# ---------------------------------------------------------------------------
# Chapter 9 — <li> with direct text (no <p> wrapper, baseline)
# ---------------------------------------------------------------------------
ch9 = xhtml("Ch9: li direct text", """
<h1>Ch 9: &lt;li&gt; Direct Text (Baseline)</h1>
<p>PASS: Each bullet below must be inline with its text. This is the baseline
case that already works — no &lt;p&gt; wrapper inside &lt;li&gt;.</p>
<ul>
<li>The fundamental difference between changes to the behavior of a system and changes to its structure.</li>
<li>The enabling magic of alternating investment in structure and investment in behavior.</li>
<li>The basics of the theory of how software design works and the forces that act on it.</li>
</ul>
<p>PASS: All three bullets above should be inline with their text.</p>
""")

CHAPTERS = [
    ("ch1", "chapter1.xhtml", "Chapter 1: Standalone br",    ch1),
    ("ch2", "chapter2.xhtml", "Chapter 2: Classed br",       ch2),
    ("ch3", "chapter3.xhtml", "Chapter 3: Multiple br",      ch3),
    ("ch4", "chapter4.xhtml", "Chapter 4: Inline br",        ch4),
    ("ch5", "chapter5.xhtml", "Chapter 5: br after heading", ch5),
    ("ch6", "chapter6.xhtml", "Chapter 6: br at start",      ch6),
    ("ch7", "chapter7.xhtml", "Chapter 7: li>p bold italic", ch7),
    ("ch8", "chapter8.xhtml", "Chapter 8: li>p nested ul",   ch8),
    ("ch9", "chapter9.xhtml", "Chapter 9: li direct text",   ch9),
]

def build_epub(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as epub:
        # mimetype must be first and uncompressed
        epub.writestr("mimetype", "application/epub+zip",
                      compress_type=zipfile.ZIP_STORED)

        epub.writestr("META-INF/container.xml", """\
<?xml version="1.0" encoding="UTF-8"?>
<container xmlns="urn:oasis:names:tc:opendocument:xmlns:container" version="1.0">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf"
              media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>""")

        epub.writestr("OEBPS/styles/test.css", CSS)

        manifest_items = []
        spine_items    = []
        nav_items      = []

        for (chid, chfile, chtitle, chcontent) in CHAPTERS:
            epub.writestr(f"OEBPS/{chfile}", chcontent)
            manifest_items.append(
                f'    <item id="{chid}" href="{chfile}" media-type="application/xhtml+xml"/>')
            spine_items.append(f'    <itemref idref="{chid}"/>')
            nav_items.append(f'      <li><a href="{chfile}">{chtitle}</a></li>')

        manifest_items.append(
            '    <item id="nav" href="nav.xhtml" '
            'media-type="application/xhtml+xml" properties="nav"/>')

        content_opf = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">test-epub-br-section-break</dc:identifier>
    <dc:title>Test: br Section Break & li/p</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
{chr(10).join(manifest_items)}
  </manifest>
  <spine>
{chr(10).join(spine_items)}
  </spine>
</package>"""
        epub.writestr("OEBPS/content.opf", content_opf)

        nav_xhtml = f"""\
<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Table of Contents</title></head>
<body>
  <nav epub:type="toc">
    <ol>
{chr(10).join(nav_items)}
    </ol>
  </nav>
</body>
</html>"""
        epub.writestr("OEBPS/nav.xhtml", nav_xhtml)

    print(f"Generated: {path}")


if __name__ == "__main__":
    build_epub(OUTPUT_PATH)
