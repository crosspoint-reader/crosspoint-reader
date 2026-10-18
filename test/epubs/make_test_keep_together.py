#!/usr/bin/env python3
"""Build test_keep_together.epub -- widow and orphan control, and headings kept with their text.

A page used to end wherever the next line no longer fit, so a paragraph's first line could sit
alone at the foot of a page (an orphan), its last line alone at the head of the next (a widow),
and a heading could end a page with its text overleaf. The parser now ends the page earlier where
that would happen, with the CSS defaults orphans: 2 and widows: 2.

The harness page is 760 px of 24 px lines: 31 lines (744 px). Each chapter fills the page with
one-line paragraphs ("Filler NN.") to a known height, then sets the case:
  1. orphan      -- 30 fillers: one line left; the 4-line ORPHANSTART paragraph moves whole
                    (it used to leave its first line behind)
  2. widow       -- 26 fillers: five lines left; the 6-line WIDOWSTART paragraph keeps four and
                    takes two over (its last line used to go over alone)
  3. no split    -- 29 fillers: two lines left; the 3-line THREELINES paragraph cannot split 2/2
                    and moves whole
  4. good split  -- 28 fillers: three lines left; the 5-line GOODSPLIT paragraph splits 3/2, as
                    before
  5. heading     -- 30 fillers: the HEADINGMOVES h2 (34 px) fits, no line of its text does; both
                    start the next page (the heading used to end this one)
  6. long        -- 30 fillers, then a LONGSTART paragraph of over 96 words, laid out in parts:
                    it starts on the next page, still splits there, and its last page holds two
                    lines or more

Regenerate with:  python test/epubs/make_test_keep_together.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build_test -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_keep_together.epub")

CSS = """p, h2 { margin: 0; text-indent: 0; }
"""

CHAPTER = """<?xml version="1.0" encoding="utf-8"?>
<html xmlns="http://www.w3.org/1999/xhtml">
  <head>
    <title>{title}</title>
    <link rel="stylesheet" type="text/css" href="style.css"/>
  </head>
  <body>
{body}
  </body>
</html>
"""

SENTENCE = "the quick brown fox jumps over the lazy dog while the reader turns another page"


def fillers(n):
    return "\n".join(f"    <p>Filler {i:02d}.</p>" for i in range(1, n + 1))


def paragraph(marker, sentences, tail=""):
    return f"    <p>{marker} " + ", and ".join([SENTENCE] * sentences) + tail + ".</p>"


CHAPTERS = [
    ("Orphan", fillers(30) + "\n" + paragraph("ORPHANSTART", 2)),
    ("Widow", fillers(26) + "\n" + paragraph("WIDOWSTART", 3)),
    ("No Split", fillers(29) + "\n" + paragraph("THREELINES", 1, ", and the quick brown fox")),
    ("Good Split", fillers(28) + "\n" + paragraph("GOODSPLIT", 2, ", and the quick brown fox jumps over the lazy dog")),
    ("Heading", fillers(30) + "\n    <h2>HEADINGMOVES</h2>\n" + paragraph("HEADINGTEXT", 3)),
    ("Long", fillers(30) + "\n" + paragraph("LONGSTART", 34)),
]


def main():
    manifest, spine, files = [], [], {}

    files["style.css"] = CSS
    manifest.append('<item id="css" href="style.css" media-type="text/css"/>')

    for i, (title, body) in enumerate(CHAPTERS, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=title, body=body)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        "    <dc:title>Keep Together</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">keep-together</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n    " + "\n    ".join(manifest) + "\n  </manifest>\n"
        "  <spine>\n    " + "\n    ".join(spine) + "\n  </spine>\n"
        "</package>\n"
    )

    container = (
        '<?xml version="1.0"?>\n'
        '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
        '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
        'media-type="application/oebps-package+xml"/></rootfiles>\n'
        "</container>\n"
    )

    # Fixed timestamps: the archive is committed, so a rebuild must be byte-identical.
    def add(z, name, data, compress=zipfile.ZIP_DEFLATED):
        info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        info.compress_type = compress
        z.writestr(info, data)

    with zipfile.ZipFile(OUT, "w") as z:
        add(z, "mimetype", "application/epub+zip", zipfile.ZIP_STORED)
        add(z, "META-INF/container.xml", container)
        add(z, "OEBPS/content.opf", opf)
        for name, content in files.items():
            add(z, "OEBPS/" + name, content)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
