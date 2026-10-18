#!/usr/bin/env python3
"""Build test_font_families.epub -- a book set in several font families (Publisher Fonts, Stage B).

The body is in one family ("Book Body", serif fallback), chapter titles, a dedication and a block quotation in a
display family ("Book Display"), one heading in the generic sans-serif, a code block in monospace, and a few blocks
in families the book does not have. What the golden variants pin (EpubPipelineTest):

  default        Off and Body Text: font-family changes no block's font (the body font ID differs in Body Text,
                 which the dump does not show)
  _accents       Accents with a built-in reader font: Book Display is the accent family (its own faces, native
                 sizes), sans-serif and a sans-serif fallback go to the built-in sans; the body family, serif (the
                 body's own generic), monospace and cursive stay the body font
  _alltext       All Text: Book Body is the body font, and Book Display the accent family beside it in one glyph
                 store (Stage B phase 2): the same layout as _accents, the body font ID aside

Per block: a block's words all in one family give the block that family (a heading set in a span, a paragraph that
is one span), else the family of the element that opened it (a body paragraph with an inline accent).

With --fonts DIR the book embeds TrueType faces for the device test (Ubuntu as Book Body, Inter as Book Display,
Noto Sans as Book Script, all from lib/EpdFont/builtinFonts/source) and is written to --out; the corpus copy
embeds none, the layout does not read them.

Regenerate with:  python test/epubs/make_test_font_families.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir <build> -R EpubPipeline
"""

import argparse
import os
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "test_font_families.epub")

CSS = """body { font-family: "Book Body", serif; }
h1, .dedication, blockquote { font-family: "Book Display", sans-serif; }
h2.generic, .sans { font-family: sans-serif; }
.helvetica { font-family: Helvetica, Arial, sans-serif; }
.georgia { font-family: Georgia, serif; }
pre, .mono { font-family: "Courier New", monospace; }
.script { font-family: "Book Script", cursive; }
.inline { font-family: "Book Display"; }
"""

FACES = [
    ("Book Body", "normal", "Ubuntu/Ubuntu-Regular.ttf", "body.ttf"),
    ("Book Body", "bold", "Ubuntu/Ubuntu-Bold.ttf", "body-bold.ttf"),
    ("Book Display", "normal", "Inter/Inter-Regular.ttf", "display.ttf"),
    ("Book Display", "bold", "Inter/Inter-Bold.ttf", "display-bold.ttf"),
    ("Book Script", "normal", "NotoSans/NotoSans-Regular.ttf", "script.ttf"),
]

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

PROSE = (
    "The body of the book is set in its own family, and most of the text is body text: paragraphs of it, one "
    "after another, which is how the reader tells the body family from the families that set a few blocks apart."
)

CHAPTERS = [
    (
        "One",
        "    <h1>Chapter One</h1>\n"
        '    <p class="dedication">For the reader who keeps the publisher\'s fonts.</p>\n'
        f"    <p>{PROSE}</p>\n"
        '    <h2 class="generic">A Heading in Sans-Serif</h2>\n'
        f"    <p>{PROSE}</p>\n"
        '    <p>A body paragraph with an <span class="inline">inline accent</span> stays in the body family.</p>\n'
        '    <p><span class="inline">A paragraph that is one span takes the span\'s family.</span></p>\n'
        "    <blockquote><p>A quotation, its family inherited from the blockquote.</p></blockquote>\n"
        "    <pre>monospace stays the reader's font</pre>\n"
        '    <p class="georgia">Georgia, with the serif the body names: the body font.</p>\n'
        '    <p class="helvetica">Helvetica, not in the book: its sans-serif fallback.</p>\n'
        '    <p class="script">A family the book has but does not use much: the body font.</p>\n'
        "    <h3>A heading in the body family</h3>\n"
        f"    <p>{PROSE}</p>",
    ),
    (
        "Two",
        '    <h1>Chapter <span class="inline">Two</span></h1>\n'
        '    <h2><span class="inline">A Heading Set in a Span</span></h2>\n'
        f'    <p class="dedication">{" ".join([PROSE] * 3)}</p>\n'
        f"    <p>{PROSE}</p>",
    ),
]


def build(out, fonts_dir):
    manifest, spine, files = [], [], {}
    css = CSS
    if fonts_dir:
        rules = []
        for i, (family, weight, src, name) in enumerate(FACES):
            with open(os.path.join(fonts_dir, src), "rb") as f:
                files["fonts/" + name] = f.read()
            manifest.append(f'<item id="font{i}" href="fonts/{name}" media-type="font/ttf"/>')
            rules.append(f'@font-face {{ font-family: "{family}"; font-weight: {weight}; src: url(fonts/{name}); }}')
        css = "\n".join(rules) + "\n" + css
    files["style.css"] = css
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
        "    <dc:title>Font Families</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        '    <dc:identifier id="bookid">font-families</dc:identifier>\n'
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

    def entry(name, data, compress):
        info = zipfile.ZipInfo(name, date_time=(2026, 10, 18, 0, 0, 0))
        info.compress_type = compress
        return info, data

    with zipfile.ZipFile(out, "w") as z:
        z.writestr(*entry("mimetype", "application/epub+zip", zipfile.ZIP_STORED))
        z.writestr(*entry("META-INF/container.xml", container, zipfile.ZIP_DEFLATED))
        z.writestr(*entry("OEBPS/content.opf", opf, zipfile.ZIP_DEFLATED))
        for name, content in files.items():
            z.writestr(*entry("OEBPS/" + name, content, zipfile.ZIP_DEFLATED))
    print("wrote", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fonts", help="embed TrueType faces from this directory (device test copy)")
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args()
    build(args.out, args.fonts)


if __name__ == "__main__":
    main()
