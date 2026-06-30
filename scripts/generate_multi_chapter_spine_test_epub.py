#!/usr/bin/env python3
"""
Generate a test EPUB with both multiple chapters inside a single XHTML spine
item (file) and chapters spanning multiple spines to verify and debug TOC /
chapter navigation.
"""

import zipfile
from pathlib import Path

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)

def create_epub(filename, title):
    """
    Create an EPUB file.
    """
    chapters_data = [
        ("Chapter 1: The Beginning", "chapter0.xhtml#chap1"),
        ("Chapter 2: The Middle", "chapter0.xhtml#chap2"),
        ("Chapter 3: The End", "chapter0.xhtml#chap3"),
        ("Chapter 4: The Epilogue", "chapter1.xhtml#chap4"),
    ]
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
        
        # We have a two body files (chapter0.xhtml and chapter1.xhtml)
        body_content_0 = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
</head>
<body>
  <h1>{title}</h1>
  <p>This EPUB tests Table of Contents navigation and chapter progress when multiple chapters are packed into a single spine file.</p>

  <h2 id="chap1">Chapter 1: The Beginning</h2>
  <p>This is Chapter 1. It contains some text to occupy pages.</p>
  <p>{"Lorem ipsum dolor sit amet, consectetur adipiscing elit. " * 21}</p>
  <p>{"Sed do eiusmod tempor incididunt ut labore et dolore magna aliqua. " * 21}</p>
  <p>{"Ut enim ad minim veniam, quis nostrud exercitation ullamco laboris nisi ut aliquip ex ea commodo consequat. " * 21}</p>

  <h2 id="chap2">Chapter 2: The Middle</h2>
  <p>This is Chapter 2. It contains some text to occupy pages.</p>
  <p>{"Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. " * 21}</p>
  <p>{"Excepteur sint occaecat cupidatat non proident, sunt in culpa qui officia deserunt mollit anim id est laborum. " * 21}</p>

  <h2 id="chap3">Chapter 3: The End</h2>
  <p>This is Chapter 3. It contains some text to occupy pages. It will spill over into the next spine file.</p>
  <p>{"Curabitur pretium tincidunt lacus. Nulla gravida orci a odio. " * 21}</p>
  <p>{"Nullam varius, turpis et commodo pharetra, est eros bibendum elit, nec luctus magna felis sollicitudin mauris. " * 21}</p>
  <p>{"Integer in mauris eu nibh euismod gravida. Duis ac tellus. " * 21}</p>
</body>
</html>'''

        body_content_1 = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{title}</title>
</head>
<body>
  <p>This is a continuation of Chapter 3 in the second spine file.</p>
  <p>{"Phasellus viverra nulla ut metus varius laoreet. Quisque rutrum. " * 21}</p>
  <p>{"Aenean imperdiet. Etiam ultricies nisi vel augue. " * 21}</p>

  <h2 id="chap4">Chapter 4: The Epilogue</h2>
  <p>This is Chapter 4. It starts in the second spine file.</p>
  <p>{"Curabitur ullamcorper ultricies nisi. Nam eget dui. " * 21}</p>
  <p>{"Etiam rhoncus. Maecenas tempus, tellus eget condimentum rhoncus. " * 21}</p>
</body>
</html>'''

        epub.writestr('OEBPS/chapter0.xhtml', body_content_0)
        epub.writestr('OEBPS/chapter1.xhtml', body_content_1)
        
        # Manifest items
        manifest_items = [
            '<item id="chapter0" href="chapter0.xhtml" media-type="application/xhtml+xml"/>',
            '<item id="chapter1" href="chapter1.xhtml" media-type="application/xhtml+xml"/>',
            '<item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
            '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
        ]
        
        # Spine items (chapter0 and chapter1)
        spine_items = [
            '<itemref idref="chapter0"/>',
            '<itemref idref="chapter1"/>'
        ]
        
        # content.opf
        epub.writestr('OEBPS/content.opf', f'''<?xml version="1.0"?>
<package version="3.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>{title}</dc:title>
    <dc:creator>CrossPoint Test Generator</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="bookid">test-multi-chapter-spine-001</dc:identifier>
  </metadata>
  <manifest>
    {''.join(manifest_items)}
  </manifest>
  <spine toc="ncx">
    {''.join(spine_items)}
  </spine>
</package>''')
        
        # toc.ncx (EPUB 2 fallback)
        nav_points = []
        for i, (ch_title, target) in enumerate(chapters_data):
            nav_points.append(f'''
    <navPoint id="navPoint-{i+1}" playOrder="{i+1}">
      <navLabel><text>{ch_title}</text></navLabel>
      <content src="{target}"/>
    </navPoint>''')
        
        epub.writestr('OEBPS/toc.ncx', f'''<?xml version="1.0"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
  <head>
    <meta name="dtb:uid" content="test-multi-chapter-spine-001"/>
  </head>
  <docTitle><text>{title}</text></docTitle>
  <navMap>
    {''.join(nav_points)}
  </navMap>
</ncx>''')

        # nav.xhtml (EPUB 3 nav)
        nav_items = []
        for ch_title, target in chapters_data:
            nav_items.append(f'      <li><a href="{target}">{ch_title}</a></li>')

        epub.writestr('OEBPS/nav.xhtml', f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Navigation</title></head>
<body>
  <nav epub:type="toc">
    <h1>Contents</h1>
    <ol>
{'\n'.join(nav_items)}
    </ol>
  </nav>
</body>
</html>''')

if __name__ == '__main__':
    print("Creating multi-chapter spine test EPUB...")
    
    output_file = OUTPUT_DIR / 'test_multi_chapter_spine.epub'
    create_epub(output_file, 'Multi-chapter Spine Test')
    print(f"Created: {output_file}")
