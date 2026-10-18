#!/usr/bin/env python3
"""Build test_block_emphasis.epub -- bold and italic set on a block reach the blocks inside it.

font-weight, font-style and font-variant are inherited, and text-decoration propagates to the text
inside, so in every reader

    <div style="font-style:italic"><p>Inner</p>tail</div>

renders "Inner" and "tail" italic. The parser kept a block's own CSS in one slot that each nested
block overwrote and each block end cleared: the <p> came out upright, and "tail" after it too.
Containers that are not block tags (<section>, <ul>) did reach their children, but from below the
block slot, so an italic <section> also overrode its own <p style="font-style:normal">. A <br> in
an italic paragraph cleared the italic for the rest of it.

What each chapter pins (the word's s= bits: 1 bold, 2 italic, 4 underline, 64 small caps):
  1. a div's italic         -- reaches its <p> and the text after it; not the paragraph after </div>
  2. overriding             -- a child's font-style:normal / font-weight:normal wins, and the
                               parent's style is back for the next child
  3. lists                  -- a bold <li> reaches its <p>; an italic <ol> its items
  4. blockquote and pre     -- an italic blockquote and a bold div reach their children
  5. section                -- an italic section, and its own upright paragraph
  6. headings               -- bold, plus an enclosing div's italic; a plain heading stays bold only
  7. a <br>                 -- the paragraph's italic carries on after it
  8. decoration and caps    -- underline and small caps on a div reach its paragraph

Every word carries an UPPERCASE marker so a golden that styles one wrongly is reviewable.

Regenerate with:  python test/epubs/make_test_block_emphasis.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir test/build_test -R EpubPipeline
"""

import os
import zipfile

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "test_block_emphasis.epub")

CSS = """p, h2, h3 { margin: 0; text-indent: 0; }
.it { font-style: italic; }
.up { font-style: normal; }
.bd { font-weight: bold; }
.light { font-weight: normal; }
blockquote { font-style: italic; }
section { font-style: italic; }
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

CHAPTERS = [
    (
        "Div Italic",
        '    <div class="it"><p>DIVINNER slanted.</p>DIVTAIL slanted too.</div>\n'
        "    <p>AFTERDIV upright.</p>",
    ),
    (
        "Overriding",
        '    <div class="it"><p class="up">UPRIGHT child.</p><p>SLANTED sibling.</p></div>\n'
        '    <div class="bd"><p class="light">LIGHT child.</p><p>HEAVY sibling.</p></div>',
    ),
    (
        "Lists",
        '    <ul><li class="bd"><p>LISTITEM bold.</p></li></ul>\n'
        '    <ol class="it"><li>OLITEM slanted.</li></ol>\n'
        "    <p>AFTERLIST upright.</p>",
    ),
    (
        "Quote And Pre",
        "    <blockquote><p>QUOTED slanted.</p></blockquote>\n"
        '    <div class="bd"><pre>PRECODE bold</pre></div>\n'
        "    <p>AFTERPRE upright.</p>",
    ),
    (
        "Section",
        '    <section><p class="up">SECTIONROMAN upright.</p><p>SECTIONOBLIQUE slanted.</p></section>',
    ),
    (
        "Headings",
        '    <div class="it"><h2>HEADING bold</h2><p>HEADBODY slanted.</p></div>\n'
        "    <h3>PLAINHEAD bold</h3>\n"
        "    <p>AFTERHEAD upright.</p>",
    ),
    (
        "Break",
        '    <p class="it">BEFOREBREAK slanted<br/>AFTERBREAK slanted.</p>\n'
        "    <p>NEXTPARA upright.</p>",
    ),
    (
        "Decoration And Caps",
        '    <div style="text-decoration:underline"><p>UNDERLINED words.</p></div>\n'
        '    <div style="font-variant:small-caps"><p>Smallcaps words.</p></div>\n'
        "    <p>PLAINEND words.</p>",
    ),
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
        "    <dc:title>Block Emphasis</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">block-emphasis</dc:identifier>\n'
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
