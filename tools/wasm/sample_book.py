"""Create the preview's deterministic, project-owned EPUB fixture."""

import json
import zipfile
from pathlib import Path


def seed_card(root: Path):
    (root / "books").mkdir(parents=True, exist_ok=True)
    (root / ".crosspoint").mkdir(exist_ok=True)
    (root / ".crosspoint/settings.json").write_text(
        json.dumps({"sleepTimeoutMinutes": 31, "language": "EN"})
    )
    chapters = ["A place to begin", "Changing the page", "One more chapter"]
    manifest = ''.join(
        f'<item id="c{i}" href="chapter{i}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(3)
    )
    spine = ''.join(f'<itemref idref="c{i}"/>' for i in range(3))
    entries = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '<?xml version="1.0"?><container version="1.0" '
        'xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile '
        'full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>'
        '</rootfiles></container>',
        "OEBPS/content.opf": '<?xml version="1.0"?><package version="2.0" '
        'xmlns="http://www.idpf.org/2007/opf" unique-identifier="id"><metadata '
        'xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:identifier id="id">crosspoint-preview-1'
        '</dc:identifier><dc:title>A Small Book of Pages</dc:title><dc:creator>CrossPoint</dc:creator>'
        '<dc:language>en</dc:language></metadata><manifest>' + manifest +
        '<item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/>'
        '</manifest><spine toc="ncx">' + spine + '</spine></package>',
        "OEBPS/toc.ncx": '<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" '
        'version="2005-1"><head><meta name="dtb:uid" content="crosspoint-preview-1"/></head>'
        '<docTitle><text>A Small Book of Pages</text></docTitle><navMap>' + ''.join(
            f'<navPoint id="c{i}" playOrder="{i+1}"><navLabel><text>{title}</text></navLabel>'
            f'<content src="chapter{i}.xhtml"/></navPoint>' for i, title in enumerate(chapters)
        ) + '</navMap></ncx>',
    }
    paragraphs = [
        "A book begins with a little space and a few words. This one lives on a virtual SD card, "
        "ready for a quiet afternoon of turning pages. There is no account to create and no device to connect.",
        "Try changing the type size, the margins, and the space between lines. A sentence that fits on "
        "one line may need two. The reader makes a new page from the same words.",
        "<em>Italic words</em> and <strong>bold words</strong> give the page a different rhythm. "
        "Punctuation matters too: a comma, a semicolon; a question? The next paragraph starts afresh.",
        "Beyond the window, a row of trees marks the edge of a field. The path turns toward a small "
        "bridge, then follows the stream. Each bend reveals a view that was hidden a moment before.",
    ]
    for i, title in enumerate(chapters):
        entries[f"OEBPS/chapter{i}.xhtml"] = (
            '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml"><head>'
            f'<title>{title}</title></head><body><h1>{title}</h1>' +
            ''.join(f'<p>{paragraphs[(j+i) % len(paragraphs)]}</p>' for j in range(24)) +
            '</body></html>'
        )
    with zipfile.ZipFile(root / "books/A Small Book of Pages.epub", "w") as book:
        for name, contents in entries.items():
            entry = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            entry.compress_type = zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            book.writestr(entry, contents.encode())
