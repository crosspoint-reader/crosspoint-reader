#!/usr/bin/env python3
"""
Generate a test EPUB that exercises the StarDict .syn synonym lookup path.

Every highlighted word below is missing from a dictionary's direct index and
resolves only by following the .syn synonym file to its real headword. Verified
against the GNU Collaborative International Dictionary of English (GCIDE), whose
StarDict build ships an 11,466-entry .syn:
  https://tuxor1337.frama.io/firedict/dictionaries.html

The chosen words are also unreachable by the mini English stemmer (no suffix
rule produces the headword), so a successful lookup proves the synonym path
specifically, not the stemming fallback.
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
        for i, (_, content) in enumerate(chapters):
            chapter_id = f'chapter{i}'
            manifest_items.append(f'<item id="{chapter_id}" href="{chapter_id}.xhtml" media-type="application/xhtml+xml"/>')
            spine_items.append(f'<itemref idref="{chapter_id}"/>')
            epub.writestr(f'OEBPS/{chapter_id}.xhtml', content)

        # content.opf
        epub.writestr('OEBPS/content.opf', f'''<?xml version="1.0"?>
<package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="bookid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:title>{title}</dc:title>
    <dc:creator>CrossPoint Test Generator</dc:creator>
    <dc:language>en</dc:language>
    <dc:identifier id="bookid">test-dictionary-synonyms-001</dc:identifier>
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
    <meta name="dtb:uid" content="test-dictionary-synonyms-001"/>
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
</head>
<body>
  <h1>{title}</h1>
  {content}
</body>
</html>'''


if __name__ == '__main__':
    print("Creating dictionary synonym-path test EPUB...")

    chapters = [
        ("Introduction", make_chapter("Synonym Path Test", """
<p>Every highlighted word below is missing from the dictionary's direct index.
It should only be found by following the GCIDE synonym (.syn) file to its real
headword. Select each word with Look Up and confirm the definition screen shows
the headword noted in parentheses.</p>
<p>These words are also unreachable by the English stemmer, so a hit proves the
synonym path specifically.</p>
""")),

        ("Spelling variants", make_chapter("Spelling Variants", """
<p>They restored the crumbling old theatre by the harbour before the winter
season, and the crowd filled every seat. (theatre &#8594; theater)</p>
<p>The king raised his jewelled sceptre above the silent hall and the herald
bowed low. (sceptre &#8594; scepter)</p>
<p>She poured a careful measure of absinthe and watched the sugar dissolve in
the green haze. (absinthe &#8594; absinth)</p>
<p>The old soldiers would accoutre themselves in worn leather and dented steel
before the march. (accoutre &#8594; accouter)</p>
""")),

        ("Contractions and older forms", make_chapter("Contractions and Older Forms", """
<p>He wandered the accurst moor for years, certain the place itself wished him
gone. (accurst &#8594; accursed)</p>
<p>The court named him an abettor to the scheme, though he swore he had only
watched. (abettor &#8594; abetter)</p>
""")),

        ("Hyphenation variant", make_chapter("Hyphenation Variant", """
<p>On the dry plain they tracked an aardwolf slipping between the termite mounds
at dusk. (aardwolf &#8594; aard-wolf)</p>
""")),

        ("Control words", make_chapter("Control Words", """
<p>These common words are direct index hits, not synonym lookups, and prove the
reader is not simply failing every word: the dog followed the cat across the
garden wall.</p>
""")),
    ]

    output_file = OUTPUT_DIR / 'test_dictionary_synonyms.epub'
    create_epub(output_file, 'Synonym Lookup Test', chapters)
    print(f"Created: {output_file}")
