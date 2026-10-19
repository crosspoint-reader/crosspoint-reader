#!/usr/bin/env python3
"""Build test_font_families.epub -- a book set in several font families (Publisher Fonts, Stage B).

The body is in one family ("Book Body", serif fallback), chapter titles, a dedication and a block quotation in a
display family ("Book Display"), as is a drop cap, a paragraph and a heading in a second accent family ("Book Script"), code in a
third ("Book Code"), one heading in the generic sans-serif, a code block in monospace, and a few blocks in families
the book does not have. What the golden variants pin (EpubPipelineTest):

  default        Off and Body Text: font-family changes no block's font (the body font ID differs in Body Text,
                 which the dump does not show)
  _accents       Accents with a built-in reader font: Book Display, Book Script and Book Code are the accent
                 families, ranked by their blocks (their own faces, native sizes); sans-serif and a sans-serif
                 fallback go to the built-in sans; the body family, serif (the body's own generic) and monospace
                 stay the body font
  _alltext       All Text: Book Body is the body font, and the accent families beside it in one glyph store: the
                 same layout as _accents, the body font ID aside

Per block: a block's words all in one family give the block that family (a heading set in a span, a paragraph that
is one span), else the family of the element that opened it (a body paragraph with an inline accent). Per word: a
word in another family than its block's has that family's face at the block's size (Stage B phase 3), as the inline
spans and the inline <code> of the mixed body paragraph do.

With --fonts DIR the book embeds TrueType faces for the device test (Ubuntu as Book Body, Inter as Book Display,
Noto Sans Italic as Book Script, Ubuntu Bold as Book Code, all from lib/EpdFont/builtinFonts/source) and is written
to --out; the corpus copy embeds none, the layout does not read them. The device copy also has a third chapter with
a baseline JPEG, a progressive JPEG and a PNG among text in every family, made here with Pillow and large enough
that their decodes need real heap: the pages the reader's memory ladder (ImageMemoryLadder) is tested on.

With --build-through (device only, needs --fonts) it writes a different book instead, "Build Through": five
chapters of prose in every family, each ending in a baseline JPEG, a progressive JPEG and a PNG of its own on its
last pages. Those are the pages a reader reaches while Background-B builds the next chapter (build-through on the
X3) and the image lane warms the pictures ahead, so the device test sees the two meet in every chapter.

Regenerate with:  python test/epubs/make_test_font_families.py
then refresh goldens: UPDATE_GOLDENS=1 ctest --test-dir <build> -R EpubPipeline
"""

import argparse
import io
import math
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
code { font-family: "Book Code", monospace; }
.initial { float: left; font-size: 3em; font-family: "Book Display"; }
"""

FACES = [
    ("Book Body", "normal", "Ubuntu/Ubuntu-Regular.ttf", "body.ttf"),
    ("Book Body", "bold", "Ubuntu/Ubuntu-Bold.ttf", "body-bold.ttf"),
    ("Book Display", "normal", "Inter/Inter-Regular.ttf", "display.ttf"),
    ("Book Display", "bold", "Inter/Inter-Bold.ttf", "display-bold.ttf"),
    ("Book Script", "normal", "NotoSans/NotoSans-Italic.ttf", "script.ttf"),
    ("Book Code", "normal", "Ubuntu/Ubuntu-Bold.ttf", "code.ttf"),
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
        '    <p class="script">A paragraph in the second accent family.</p>\n'
        "    <h3>A heading in the body family</h3>\n"
        f"    <p>{PROSE}</p>\n"
        '    <p>Body text with an <span class="inline">accent</span>, a <span class="script">second accent</span> '
        "and <code>inline code</code> in it, each in its own family.</p>\n"
        '    <h2 class="script">A Heading in the Second Accent</h2>\n'
        "    <pre><code>code in the code family</code></pre>",
    ),
    (
        "Two",
        '    <h1>Chapter <span class="inline">Two</span></h1>\n'
        '    <h2><span class="inline">A Heading Set in a Span</span></h2>\n'
        f'    <p class="dedication">{" ".join([PROSE] * 3)}</p>\n'
        f'    <p><span class="initial">{PROSE[0]}</span>{PROSE[1:]}</p>',  # a drop cap in the accent family
    ),
]


# The device copy's chapter with pictures: each image on a page with text in the body and accent families, so that
# drawing the page needs every family of the set and the decode.
IMAGE_CHAPTER = (
    "Three",
    "    <h1>Chapter Three</h1>\n"
    '    <p class="dedication">Pictures among the publisher\'s fonts.</p>\n'
    '    <p><img src="images/baseline.jpg" alt="A baseline JPEG"/></p>\n'
    '    <p class="script">A paragraph in the second accent family, under a baseline JPEG.</p>\n'
    "    <p>Body text with <code>inline code</code> and an <span class=\"inline\">accent</span> after it.</p>\n"
    '    <h2 class="script">A Progressive Picture</h2>\n'
    '    <p><img src="images/progressive.jpg" alt="A progressive JPEG"/></p>\n'
    f"    <p>{PROSE}</p>\n"
    "    <h1>A Figure</h1>\n"
    '    <p><img src="images/figure.png" alt="A PNG figure"/></p>\n'
    "    <pre><code>code in the code family</code></pre>\n"
    f"    <p>{PROSE}</p>",
)


def picture(width, height, phase):
    """Smooth shapes over gradients, the same pixels every run."""
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        raise SystemExit("--fonts needs Pillow for the test images: pip install pillow")
    image = Image.new("RGB", (width, height))
    pixels = image.load()
    for y in range(height):
        for x in range(width):
            r = int(127 + 127 * math.sin((x + phase) / 37.0))
            g = int(127 + 127 * math.sin((y - phase) / 23.0))
            b = (x * 255 // width + y * 255 // height) // 2
            pixels[x, y] = (r, g, b)
    draw = ImageDraw.Draw(image)
    for i in range(12):
        cx = (i * 97 + phase) % width
        cy = (i * 61 + phase * 2) % height
        draw.ellipse([cx - 40, cy - 40, cx + 40, cy + 40], outline=(0, 0, 0), width=4)
    return image


def saved(image, **options):
    buffer = io.BytesIO()
    image.save(buffer, **options)
    return buffer.getvalue()


def make_images():
    """A baseline JPEG, a progressive JPEG and a PNG."""
    return {
        "images/baseline.jpg": saved(picture(900, 675, 0), format="JPEG", quality=80, optimize=False),
        "images/progressive.jpg": saved(picture(800, 600, 40), format="JPEG", quality=80, progressive=True),
        "images/figure.png": saved(picture(640, 480, 80), format="PNG"),
    }


# "Build Through": every chapter is long enough that Background-B's build of it spans several page turns on the X3
# (~90 pages, a few seconds of slices), and ends in three pictures of its own, each on a page of its own, among the
# last pages.
BUILD_THROUGH_CHAPTERS = 5
BUILD_THROUGH_PARAGRAPHS = 160
BUILD_THROUGH_PROSE = [
    f"{PROSE} Now and then a word is set apart, like <span class=\"inline\">this display phrase</span>, a "
    f"<span class=\"script\">turn of the script hand</span> or a little <code>code</code>, and the body goes on "
    f"after it as before, line after line, until the paragraph ends.",
    f"{PROSE} {PROSE}",
    f"A shorter paragraph in the body family, with <span class=\"inline\">one accent</span> in it.",
]


def build_through_chapter(n):
    prose = "\n".join(f"    <p>{BUILD_THROUGH_PROSE[i % len(BUILD_THROUGH_PROSE)]}</p>" for i in range(BUILD_THROUGH_PARAGRAPHS))
    return (
        f"Through {n}",
        f"    <h1>Chapter {n}</h1>\n"
        f'    <p class="dedication">The pictures of chapter {n} wait at its end.</p>\n'
        f"{prose}\n"
        f'    <p><img src="images/c{n}-baseline.jpg" alt="Chapter {n}, a baseline JPEG"/></p>\n'
        f'    <p class="script">Under the baseline picture of chapter {n}.</p>\n'
        f'    <p><img src="images/c{n}-progressive.jpg" alt="Chapter {n}, a progressive JPEG"/></p>\n'
        f"    <p>{PROSE}</p>\n"
        f'    <p><img src="images/c{n}-figure.png" alt="Chapter {n}, a PNG figure"/></p>\n'
        f"    <p>The last words of chapter {n}.</p>",
    )


def build_through_images(n):
    phase = n * 53
    return {
        f"images/c{n}-baseline.jpg": saved(picture(1000, 1333, phase), format="JPEG", quality=85, optimize=False),
        f"images/c{n}-progressive.jpg": saved(
            picture(1000, 750, phase + 40), format="JPEG", quality=85, progressive=True
        ),
        f"images/c{n}-figure.png": saved(picture(640, 853, phase + 80), format="PNG"),
    }


def build(out, fonts_dir, build_through=False):
    manifest, spine, files = [], [], {}
    css = CSS
    chapters = CHAPTERS + ([IMAGE_CHAPTER] if fonts_dir else [])
    title, ident = "Font Families", "font-families"
    if fonts_dir:
        rules = []
        for i, (family, weight, src, name) in enumerate(FACES):
            with open(os.path.join(fonts_dir, src), "rb") as f:
                files["fonts/" + name] = f.read()
            manifest.append(f'<item id="font{i}" href="fonts/{name}" media-type="font/ttf"/>')
            rules.append(f'@font-face {{ font-family: "{family}"; font-weight: {weight}; src: url(fonts/{name}); }}')
        css = "\n".join(rules) + "\n" + css
        images = make_images()
        if build_through:
            chapters = [build_through_chapter(n) for n in range(1, BUILD_THROUGH_CHAPTERS + 1)]
            images = {}
            for n in range(1, BUILD_THROUGH_CHAPTERS + 1):
                images.update(build_through_images(n))
            title, ident = "Build Through", "build-through"
        for i, (name, data) in enumerate(images.items()):
            files[name] = data
            media = "image/png" if name.endswith(".png") else "image/jpeg"
            manifest.append(f'<item id="image{i}" href="{name}" media-type="{media}"/>')
    files["style.css"] = css
    manifest.append('<item id="css" href="style.css" media-type="text/css"/>')
    for i, (chapter_title, body) in enumerate(chapters, start=1):
        name = f"chapter{i}.xhtml"
        files[name] = CHAPTER.format(title=chapter_title, body=body)
        manifest.append(f'<item id="chapter{i}" href="{name}" media-type="application/xhtml+xml"/>')
        spine.append(f'<itemref idref="chapter{i}"/>')

    opf = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        "    <dc:language>en</dc:language>\n"
        f'    <dc:identifier id="bookid">{ident}</dc:identifier>\n'
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
            # Images are stored, as books store them: they do not deflate.
            stored = name.startswith("images/")
            z.writestr(*entry("OEBPS/" + name, content, zipfile.ZIP_STORED if stored else zipfile.ZIP_DEFLATED))
    print("wrote", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--fonts", help="embed TrueType faces from this directory (device test copy)")
    parser.add_argument("--out", default=OUT)
    parser.add_argument("--build-through", action="store_true", help="write the Build Through book (needs --fonts)")
    args = parser.parse_args()
    if args.build_through and not args.fonts:
        parser.error("--build-through needs --fonts")
    build(args.out, args.fonts, args.build_through)


if __name__ == "__main__":
    main()
