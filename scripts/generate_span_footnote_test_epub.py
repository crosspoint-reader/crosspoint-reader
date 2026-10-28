#!/usr/bin/env python3
"""Generate a Russian EPUB with 151 non-TOC span footnote destinations."""

import argparse
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile


def xhtml(title, body):
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<html xmlns="http://www.w3.org/1999/xhtml" lang="ru">
<head><title>{title}</title></head><body>{body}</body></html>'''


def generate(output):
    links = ''.join(
        f'<p>Сноска {i}: <a href="notes.xhtml#id{i}">{i}</a></p>'
        for i in (1, 28, 151, *[n for n in range(2, 151) if n != 28])
    )
    converter_ids = ''.join(f'<span id="kobo-{i}"></span>' for i in range(10000))
    note_text = 'Текст примечания для проверки перехода на нужную страницу. ' * 12
    notes = ''.join(
        f'<span id="id{i}"><div><p>Примечание {i}</p></div><p>{note_text}</p></span>'
        for i in range(1, 152)
    )
    output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(output, 'w', ZIP_DEFLATED) as epub:
        epub.writestr('mimetype', 'application/epub+zip', compress_type=ZIP_STORED)
        epub.writestr('META-INF/container.xml', '''<?xml version="1.0"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
<rootfiles><rootfile full-path="OPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles>
</container>''')
        epub.writestr('OPS/content.opf', '''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="2.0" unique-identifier="bookid">
<metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
<dc:identifier id="bookid">crosspoint-span-footnotes-3798</dc:identifier>
<dc:title>Проверка сносок на span</dc:title><dc:creator>CrossPoint tests</dc:creator>
<dc:language>ru</dc:language></metadata>
<manifest><item id="main" href="main.xhtml" media-type="application/xhtml+xml"/>
<item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/>
<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest>
<spine toc="ncx"><itemref idref="main"/><itemref idref="notes"/></spine></package>''')
        epub.writestr('OPS/toc.ncx', '''<?xml version="1.0" encoding="UTF-8"?>
<ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1">
<head><meta name="dtb:uid" content="crosspoint-span-footnotes-3798"/></head>
<docTitle><text>Проверка сносок на span</text></docTitle><navMap>
<navPoint id="main" playOrder="1"><navLabel><text>Текст</text></navLabel>
<content src="main.xhtml"/></navPoint>
<navPoint id="notes" playOrder="2"><navLabel><text>Примечания</text></navLabel>
<content src="notes.xhtml"/></navPoint></navMap></ncx>''')
        epub.writestr('OPS/main.xhtml', xhtml('Текст', '<h1>Проверка сносок</h1>' + links))
        epub.writestr('OPS/notes.xhtml', xhtml('Примечания', converter_ids + notes))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, nargs='?', default=Path('build/test_span_footnotes.epub'))
    args = parser.parse_args()
    generate(args.output)
    print(args.output.resolve())
