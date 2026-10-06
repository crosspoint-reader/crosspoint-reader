"""Create small, deterministic files for on-device CLI regression testing."""

import argparse
import hashlib
import json
import struct
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape


def png(width, height, pixels):
    import zlib

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data))
        )

    rows = b"".join(b"\0" + pixels[y * width : (y + 1) * width] for y in range(height))
    return (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows))
        + chunk(b"IEND", b"")
    )


def make_epub(path, picture):
    paragraphs = "".join(
        f"<p>Paragraph {i}. Apple starlight reader chapter. This fixed passage tests page changes, "
        "bookmarks, line wrapping, and return navigation.</p>"
        for i in range(1, 61)
    )
    chapters = [
        (
            "Controls and footnotes",
            "<h1>Controls and footnotes</h1><p>Apple starlight reader chapter.</p>"
            '<p>This first page has a note <a epub:type="noteref" href="notes.xhtml#note-one">[1]</a>.</p>'
            "<p>Use the reader menu to open Footnotes, Dictionary, Bookmarks, and QR.</p>"
            + paragraphs,
        ),
        (
            "Images and layout",
            "<h1>Images and layout</h1><p>The pattern has a black upper-left block and stripes.</p>"
            '<img src="pattern.png" alt="Asymmetric test pattern"/><p>After the image.</p>'
            + paragraphs,
        ),
        (
            "Final chapter",
            "<h1>Final chapter</h1><p>Last chapter for percentage and end-of-book navigation.</p>"
            + paragraphs,
        ),
    ]
    content = {
        "mimetype": "application/epub+zip",
        "META-INF/container.xml": '<?xml version="1.0"?><container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container"><rootfiles><rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/></rootfiles></container>',
        "OEBPS/pattern.png": picture,
        "OEBPS/notes.xhtml": '<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>Notes</title></head><body><h1>Notes</h1><aside epub:type="footnote" id="note-one"><p>Footnote one: the destination is correct. <a href="chapter1.xhtml">Return</a></p></aside></body></html>',
    }
    for i, (title, body) in enumerate(chapters, 1):
        content[f"OEBPS/chapter{i}.xhtml"] = (
            f'<?xml version="1.0"?><html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops"><head><title>{escape(title)}</title></head><body>{body}</body></html>'
        )
    items = "".join(
        f'<item id="c{i}" href="chapter{i}.xhtml" media-type="application/xhtml+xml"/>'
        for i in range(1, 4)
    )
    content["OEBPS/content.opf"] = (
        '<?xml version="1.0"?><package version="2.0" xmlns="http://www.idpf.org/2007/opf" unique-identifier="id"><metadata xmlns:dc="http://purl.org/dc/elements/1.1/"><dc:title>CLI Regression Fixture</dc:title><dc:creator>CrossPoint Test Fixture</dc:creator><dc:language>en</dc:language><dc:identifier id="id">urn:crosspoint:cli-fixture:v1</dc:identifier></metadata><manifest>'
        + items
        + '<item id="notes" href="notes.xhtml" media-type="application/xhtml+xml"/><item id="picture" href="pattern.png" media-type="image/png"/><item id="ncx" href="toc.ncx" media-type="application/x-dtbncx+xml"/></manifest><spine toc="ncx"><itemref idref="c1"/><itemref idref="c2"/><itemref idref="c3"/><itemref idref="notes" linear="no"/></spine></package>'
    )
    points = "".join(
        f'<navPoint id="n{i}" playOrder="{i}"><navLabel><text>{escape(title)}</text></navLabel><content src="chapter{i}.xhtml"/></navPoint>'
        for i, (title, _) in enumerate(chapters, 1)
    )
    content["OEBPS/toc.ncx"] = (
        '<?xml version="1.0"?><ncx xmlns="http://www.daisy.org/z3986/2005/ncx/" version="2005-1"><head><meta name="dtb:uid" content="urn:crosspoint:cli-fixture:v1"/></head><docTitle><text>CLI Regression Fixture</text></docTitle><navMap>'
        + points
        + "</navMap></ncx>"
    )
    with zipfile.ZipFile(path, "w") as book:
        for name, data in content.items():
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = (
                zipfile.ZIP_STORED if name == "mimetype" else zipfile.ZIP_DEFLATED
            )
            book.writestr(info, data)


def make_xtc(path):
    width, height, count = 528, 792, 3
    chapters = b"".join(
        name.encode().ljust(80, b"\0") + struct.pack("<HH", start, end) + bytes(12)
        for name, start, end in [("First chapter", 1, 2), ("Last chapter", 3, 3)]
    )
    table_offset = 56 + len(chapters)
    data_offset = table_offset + count * 16
    pages = []
    for page in range(count):
        pixels = bytearray(b"\xff" * ((width + 7) // 8 * height))
        for y in range(height):
            for x in range(width):
                black = (
                    x < 8
                    or y < 8
                    or x >= width - 8
                    or y >= height - 8
                    or (24 < y < 64 and 24 < x < 80 * (page + 1))
                    or (100 < y < 500 and (x // 32 + y // 32 + page) % 2 == 0)
                )
                if black:
                    pixels[y * (width // 8) + x // 8] &= ~(0x80 >> (x % 8))
        pages.append(
            struct.pack("<IHHBBIQ", 0x00475458, width, height, 0, 0, len(pixels), 0)
            + pixels
        )
    header = struct.pack(
        "<IBBHBBBBIQQQQII",
        0x00435458,
        1,
        0,
        count,
        0,
        0,
        0,
        1,
        1,
        0,
        table_offset,
        data_offset,
        0,
        56,
        0,
    )
    entries = []
    offset = data_offset
    for page in pages:
        entries.append(struct.pack("<QIHH", offset, len(page), width, height))
        offset += len(page)
    path.write_bytes(header + chapters + b"".join(entries) + b"".join(pages))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    root = parser.parse_args().output
    root.mkdir(parents=True, exist_ok=True)
    pixels = bytes(
        0 if (x < 24 and y < 24) or (y > 40 and x // 16 % 2 == 0) else 255
        for y in range(96)
        for x in range(128)
    )
    picture = png(128, 96, pixels)
    (root / "pattern.png").write_bytes(picture)
    body = b"".join(
        bytes(v for gray in pixels[y * 128 : (y + 1) * 128] for v in (gray, gray, gray))
        for y in range(95, -1, -1)
    )
    (root / "pattern.bmp").write_bytes(
        b"BM"
        + struct.pack("<IHHI", 54 + len(body), 0, 0, 54)
        + struct.pack(
            "<IiiHHIIiiII", 40, 128, 96, 1, 24, 0, len(body), 2835, 2835, 0, 0
        )
        + body
    )
    text = "CLI text fixture. Apple starlight reader chapter.\n\n" + "".join(
        f"Line {i}: Repeatable page navigation and persistence checks.\n"
        for i in range(1, 181)
    )
    (root / "reading.txt").write_text(text)
    (root / "reading.md").write_text("# CLI Markdown fixture\n\n" + text)
    (root / "empty.txt").write_bytes(b"")
    make_epub(root / "controls.epub", picture)
    make_xtc(root / "chapters.xtc")
    invalid = root / "invalid"
    invalid.mkdir(exist_ok=True)
    (invalid / "broken.epub").write_bytes(b"This is deliberately not a ZIP archive.\n")
    (invalid / "invalid-firmware.bin").write_bytes(
        b"NOT ESP FIRMWARE - VALIDATION TEST ONLY\n"
    )
    dictionary = root / "dictionary"
    dictionary.mkdir(exist_ok=True)
    entries = {
        "apple": "A round fruit. CLI test definition for apple.",
        "chapter": "A division of a book. CLI test definition for chapter.",
        "reader": "A person or device that reads. CLI test definition for reader.",
        "starlight": "Light from stars. CLI test definition for starlight.",
    }
    idx = bytearray()
    data = bytearray()
    for word, definition in sorted(entries.items()):
        encoded = definition.encode()
        idx.extend(word.encode() + b"\0" + struct.pack(">II", len(data), len(encoded)))
        data.extend(encoded)
    (dictionary / "cli.idx").write_bytes(idx)
    (dictionary / "cli.dict").write_bytes(data)
    (dictionary / "cli.syn").write_bytes(b"apples\0" + struct.pack(">I", 0))
    (dictionary / "cli.ifo").write_text(
        "StarDict's dict ifo file\nversion=2.4.2\nbookname=CLI Test Dictionary\nwordcount=4\nsynwordcount=1\nidxfilesize="
        + str(len(idx))
        + "\nsametypesequence=m\n"
    )
    files = [
        {
            "file": str(p.relative_to(root)),
            "bytes": p.stat().st_size,
            "sha256": hashlib.sha256(p.read_bytes()).hexdigest(),
        }
        for p in sorted(root.rglob("*"))
        if p.is_file() and p.name != "manifest.json"
    ]
    (root / "manifest.json").write_text(
        json.dumps({"version": 1, "files": files}, indent=2) + "\n"
    )
    print(f"Created {len(files)} fixture files in {root}")


if __name__ == "__main__":
    main()
