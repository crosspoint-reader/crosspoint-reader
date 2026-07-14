#!/usr/bin/env python3
"""
Generate test EPUB for subscript and superscript rendering verification.
"""

import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

def create_epub(filename, title, chapters):
    """Create an EPUB file with given chapters."""
    with zipfile.ZipFile(filename, 'w', zipfile.ZIP_DEFLATED) as epub:
        # mimetype (uncompressed, first file)
        epub.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)
        
        # META-INF/container.xml
        epub.writestr('META-INF/container.xml', '''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>''')
        
        # Build spine and manifest
        manifest_items = []
        spine_items = []
        for i, (chapter_title, content) in enumerate(chapters):
            chapter_id = f'chapter{i}'
            manifest_items.append(f'<item id="{chapter_id}" href="{chapter_id}.xhtml" media-type="application/xhtml+xml"/>')
            spine_items.append(f'<itemref idref="{chapter_id}"/>')
            epub.writestr(f'OEBPS/{chapter_id}.xhtml', content)
        
        # Write stylesheet
        epub.writestr('OEBPS/style.css', '''.super {
  vertical-align: super;
}
.sub {
  vertical-align: sub;
}
''')
        manifest_items.append('<item id="style" href="style.css" media-type="text/css"/>')
        
        # content.opf
        epub.writestr('OEBPS/content.opf', f'''<?xml version="1.0"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>{title}</dc:title>
    <dc:creator>CrossPoint Test Generator</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="bookid">test-supsub-001</dc:identifier>
  </metadata>
  <manifest>
    {''.join(manifest_items)}
    <item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>
  </manifest>
  <spine toc="ncx">
    {''.join(spine_items)}
  </spine>
</package>''')
        
        # toc.ncx
        nav_points = []
        for i, (chapter_title, _) in enumerate(chapters):
            nav_points.append(f'''
    <navPoint id="navPoint-{i+1}" playOrder="{i+1}">
      <navLabel><text>{chapter_title}</text></navLabel>
      <content src="chapter{i}.xhtml"/>
    </navPoint>''')
        
        epub.writestr('OEBPS/toc.ncx', f'''<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="test-supsub-001"/>
  </head>
  <docTitle><text>{title}</text></docTitle>
  <navMap>
    {''.join(nav_points)}
  </navMap>
</ncx>''')

def make_chapter(title, content):
    """Create XHTML chapter content."""
    return f'''<?xml version="1.0" encoding="UTF-8"?>
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
</html>'''

if __name__ == '__main__':
    print("Creating subscript/superscript test EPUB...")
    
    chapters = [
        ("Introduction", make_chapter("Subscript and Superscript Tests", """
<p>This EPUB tests subscript and superscript rendering.</p>
<p>Features tested:</p>
<ul>
<li>Basic superscript (exponents, footnotes)</li>
<li>Basic subscript (chemical formulas, sequences)</li>
<li>Mixed sup and sub in same paragraph</li>
<li>Ordinal numbers</li>
<li>Nested with bold and italic</li>
<li>Long runs of sup/sub text</li>
</ul>
""")),
        
        ("Basic Superscript", make_chapter("Basic Superscript", """
<h2>Mathematical Formulas</h2>
<p>E = mc<sup>2</sup> is Einstein's mass-energy equivalence.</p>
<p>The area of a circle is πr<sup>2</sup>.</p>
<p>2<sup>10</sup> = 1024.</p>
<p>x<sup>n</sup> + y<sup>n</sup> = z<sup>n</sup></p>

<h2>Footnote References</h2>
<p>This is a sentence with a footnote<sup>1</sup> reference.</p>
<p>Multiple footnotes<sup>2</sup> can appear<sup>3</sup> in one paragraph.</p>
""")),
        
        ("Basic Subscript", make_chapter("Basic Subscript", """
<h2>Chemical Formulas</h2>
<p>Water is H<sub>2</sub>O.</p>
<p>Carbon dioxide is CO<sub>2</sub>.</p>
<p>Glucose: C<sub>6</sub>H<sub>12</sub>O<sub>6</sub>.</p>
<p>Sulfuric acid: H<sub>2</sub>SO<sub>4</sub>.</p>

<h2>Mathematical Sequences</h2>
<p>The sequence a<sub>1</sub>, a<sub>2</sub>, a<sub>3</sub>, ..., a<sub>n</sub>.</p>
<p>Matrix element A<sub>ij</sub> where i is row and j is column.</p>
""")),
        
        ("Mixed Sup and Sub", make_chapter("Mixed Superscript and Subscript", """
<h2>Chemistry and Physics</h2>
<p>The pH of water is 7, meaning [H<sub>3</sub>O<sup>+</sup>] = 10<sup>-7</sup> mol/L.</p>
<p>Speed of light: c = 2.998 × 10<sup>8</sup> m/s.</p>
<p>Avogadro's number: 6.022 × 10<sup>23</sup> mol<sup>-1</sup>.</p>

<h2>Complex Formulas</h2>
<p>Isotope notation: <sup>235</sup>U<sub>92</sub> (uranium-235).</p>
<p>Electron configuration: 1s<sup>2</sup> 2s<sup>2</sup> 2p<sup>6</sup>.</p>
""")),
        
        ("Ordinals and Dates", make_chapter("Ordinal Numbers", """
<h2>Ordinal Suffixes</h2>
<p>On the 1<sup>st</sup> of January, the 2<sup>nd</sup> quarter begins on the 3<sup>rd</sup> month.</p>
<p>The 4<sup>th</sup> through 20<sup>th</sup> days of the month.</p>
<p>The 21<sup>st</sup>, 22<sup>nd</sup>, 23<sup>rd</sup>, and 24<sup>th</sup> hours.</p>

<h2>Historical Dates</h2>
<p>The 19<sup>th</sup> century saw rapid industrialization.</p>
<p>World War II ended on May 8<sup>th</sup>, 1945.</p>
""")),
        
        ("Nested Styles", make_chapter("Nested with Bold and Italic", """
<h2>Bold Superscript</h2>
<p>Bold superscript: x<sup><b>2</b></sup> + y<sup><b>2</b></sup> = r<sup><b>2</b></sup>.</p>
<p>Bold base with superscript: <b>E = mc<sup>2</sup></b>.</p>

<h2>Italic Subscript</h2>
<p>Italic subscript: H<sub><i>n</i></sub> represents the n-th harmonic.</p>
<p>Italic base with subscript: <i>a<sub>1</sub>, a<sub>2</sub>, a<sub>3</sub></i>.</p>

<h2>Combined Styles</h2>
<p><b>Bold text with H<sub>2</sub>O and E = mc<sup>2</sup> inside.</b></p>
<p><i>Italic text with CO<sub>2</sub> and x<sup>n</sup> inside.</i></p>
""")),
        
        ("Long Runs", make_chapter("Long Superscript and Subscript Runs", """
<h2>Extended Superscript</h2>
<p>This word<sup>has a rather long superscript attached to it</sup> continuing normally.</p>
<p>Polynomial: x<sup>10</sup> + x<sup>9</sup> + x<sup>8</sup> + x<sup>7</sup> + x<sup>6</sup> + x<sup>5</sup> + x<sup>4</sup> + x<sup>3</sup> + x<sup>2</sup> + x + 1.</p>

<h2>Extended Subscript</h2>
<p>This word<sub>has a rather long subscript attached to it</sub> continuing normally.</p>
<p>Sequence: a<sub>1</sub>, a<sub>2</sub>, a<sub>3</sub>, a<sub>4</sub>, a<sub>5</sub>, a<sub>6</sub>, a<sub>7</sub>, a<sub>8</sub>, a<sub>9</sub>, a<sub>10</sub>.</p>

<h2>Alternating</h2>
<p>Mixed: x<sup>2</sup>y<sub>1</sub> + x<sup>3</sup>y<sub>2</sub> + x<sup>4</sup>y<sub>3</sub> = 0.</p>
""")),
        
        ("Edge Cases", make_chapter("Edge Cases and Stress Tests", """
<h2>Empty and Whitespace</h2>
<p>Empty superscript: x<sup></sup>y should show xy.</p>
<p>Whitespace: x<sup> </sup>y should show x y.</p>

<h2>Nested Sup/Sub (Not Standard)</h2>
<p>Nested attempt: x<sup>y<sup>z</sup></sup> (may not render correctly).</p>

<h2>Unicode in Sup/Sub</h2>
<p>Greek letters: α<sup>2</sup> + β<sup>2</sup> = γ<sup>2</sup>.</p>
<p>Symbols: H<sub>2</sub>O → H<sup>+</sup> + OH<sup>−</sup>.</p>

<h2>Line Breaking</h2>
<p>Long text with superscript at the end of a line to test wrapping behavior when the superscript might need to wrap to the next line<sup>1</sup> and continue here.</p>
""")),
        
        ("CSS Style Tests", make_chapter("CSS Style Tests", """
<h2>CSS Classes</h2>
<p>This tests superscript via CSS class (.super): E = mc<span class="super">2</span>.</p>
<p>This tests subscript via CSS class (.sub): Water is H<span class="sub">2</span>O.</p>

<h2>CSS Inline Styles</h2>
<p>This tests superscript via inline style: E = mc<span style="vertical-align: super;">2</span>.</p>
<p>This tests subscript via inline style: Water is H<span style="vertical-align: sub;">2</span>O.</p>

<h2>Comparison Side-by-Side</h2>
<p>Tag `sup`: E = mc<sup>2</sup></p>
<p>Class `super`: E = mc<span class="super">2</span></p>
<p>Inline `super`: E = mc<span style="vertical-align: super;">2</span></p>
<br/>
<p>Tag `sub`: H<sub>2</sub>O</p>
<p>Class `sub`: H<span class="sub">2</span>O</p>
<p>Inline `sub`: H<span style="vertical-align: sub;">2</span>O</p>

<h2>Mixed and Nested in CSS</h2>
<p>CSS Class mixed: [H<span class="sub">3</span>O<span class="super">+</span>] = 10<span class="super">-7</span> mol/L.</p>
<p>CSS Inline mixed: [H<span style="vertical-align: sub;">3</span>O<span style="vertical-align: super;">+</span>] = 10<span style="vertical-align: super;">-7</span> mol/L.</p>
<p><b>Bold text with class H<span class="sub">2</span>O and E = mc<span class="super">2</span> inside.</b></p>
<p><i>Italic text with inline H<span style="vertical-align: sub;">2</span>O and E = mc<span style="vertical-align: super;">2</span> inside.</i></p>
""")),
    ]
    
    output_file = OUTPUT_DIR / 'test_supsub.epub'
    create_epub(output_file, 'Subscript and Superscript Tests', chapters)
    print(f"Created: {output_file}")
