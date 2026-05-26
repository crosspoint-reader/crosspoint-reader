#!/usr/bin/env python3
"""Generate an EPUB from USER_GUIDE.md."""

import html as _html
import io
import re
from pathlib import Path

import markdown
from ebooklib import epub
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).parent.parent
SOURCE_MD = ROOT / "USER_GUIDE.md"
OUTPUT_EPUB = ROOT / "CrossPoint_User_Guide.epub"
LOGO_PNG = ROOT / "src/images/Logo120.png"

# Portrait cover dimensions matching the X4 display (480×800)
COVER_W, COVER_H = 480, 800

CSS = """
body {
    font-family: Georgia, 'Times New Roman', serif;
    font-size: 1em;
    line-height: 1.6;
    margin: 1em 1.5em;
    color: #111;
}

h1 { font-size: 1.8em; margin-top: 1.2em; margin-bottom: 0.4em; }
h2 { font-size: 1.4em; margin-top: 1.5em; margin-bottom: 0.3em; border-bottom: 1px solid #ccc; padding-bottom: 0.2em; }
h3 { font-size: 1.2em; margin-top: 1.2em; margin-bottom: 0.2em; }
h4 { font-size: 1.05em; margin-top: 1em; margin-bottom: 0.2em; }
h5 { font-size: 1em; margin-top: 0.8em; margin-bottom: 0.2em; }

p { margin: 0.6em 0; }

code {
    font-family: 'Courier New', Courier, monospace;
    font-size: 0.85em;
    background: #f4f4f4;
    padding: 0.1em 0.3em;
    border-radius: 3px;
}

pre {
    font-family: 'Courier New', Courier, monospace;
    font-size: 0.8em;
    background: #f4f4f4;
    border: 1px solid #ddd;
    border-radius: 4px;
    padding: 0.8em 1em;
    overflow-x: auto;
    white-space: pre-wrap;
    word-wrap: break-word;
}

pre code {
    background: none;
    padding: 0;
    border-radius: 0;
    font-size: 1em;
}

table {
    border-collapse: collapse;
    width: 100%;
    margin: 0.8em 0;
    font-size: 0.9em;
}

th, td {
    border: 1px solid #ccc;
    padding: 0.4em 0.6em;
    text-align: left;
}

th { background: #eee; font-weight: bold; }

ul, ol { margin: 0.5em 0; padding-left: 1.8em; }
li { margin: 0.25em 0; }

a { color: #1a6699; text-decoration: none; }

hr { border: none; border-top: 1px solid #ccc; margin: 1.5em 0; }

.callout {
    border-left: 4px solid #888;
    background: #f8f8f8;
    padding: 0.5em 0.8em;
    margin: 0.8em 0;
    border-radius: 0 4px 4px 0;
}

.callout-note   { border-color: #2196F3; background: #e3f2fd; }
.callout-tip    { border-color: #4CAF50; background: #e8f5e9; }
.callout-warning { border-color: #FF9800; background: #fff3e0; }

.callout-title {
    font-weight: bold;
    margin-bottom: 0.3em;
}

/* Cover page */
.cover-page {
    text-align: center;
    margin: 0;
    padding: 0;
}

.cover-page img {
    width: 100%;
    height: auto;
    display: block;
    margin: 0;
}
"""


def make_cover_png() -> bytes:
    """Generate a full-size (480×800) cover image with logo, title, and subtitle."""
    cover = Image.new('RGB', (COVER_W, COVER_H), color=(255, 255, 255))
    draw = ImageDraw.Draw(cover)

    # Paste logo centred at ~35% down the canvas
    with Image.open(LOGO_PNG) as logo_raw:
        if logo_raw.mode in ('RGBA', 'LA', 'P'):
            bg = Image.new('RGB', logo_raw.size, (255, 255, 255))
            bg.paste(logo_raw, mask=logo_raw.convert('RGBA').split()[3])
            logo = bg
        else:
            logo = logo_raw.convert('RGB')

    # Scale logo to 240×240 (half the cover width)
    logo = logo.resize((240, 240), Image.LANCZOS)
    logo_x = (COVER_W - 240) // 2
    logo_y = int(COVER_H * 0.28)
    cover.paste(logo, (logo_x, logo_y))

    # Try to use a system font; fall back to default if unavailable
    try:
        font_title = ImageFont.truetype('/usr/share/fonts/truetype/liberation/LiberationSerif-Bold.ttf', 52)
        font_sub = ImageFont.truetype('/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf', 32)
    except OSError:
        font_title = ImageFont.load_default()
        font_sub = font_title

    title = 'CrossPoint'
    subtitle = 'User Guide'

    # Draw title text
    bbox = draw.textbbox((0, 0), title, font=font_title)
    tw = bbox[2] - bbox[0]
    ty = logo_y + 240 + 48
    draw.text(((COVER_W - tw) // 2, ty), title, fill=(0, 0, 0), font=font_title)

    # Draw subtitle text
    bbox2 = draw.textbbox((0, 0), subtitle, font=font_sub)
    sw = bbox2[2] - bbox2[0]
    sy = ty + (bbox[3] - bbox[1]) + 18
    draw.text(((COVER_W - sw) // 2, sy), subtitle, fill=(80, 80, 80), font=font_sub)

    # Thin horizontal rules above and below the text block
    rule_y1 = ty - 24
    rule_y2 = sy + (bbox2[3] - bbox2[1]) + 24
    draw.line([(60, rule_y1), (COVER_W - 60, rule_y1)], fill=(180, 180, 180), width=1)
    draw.line([(60, rule_y2), (COVER_W - 60, rule_y2)], fill=(180, 180, 180), width=1)

    buf = io.BytesIO()
    cover.save(buf, format='PNG')
    return buf.getvalue()


def preprocess_callouts(text: str) -> str:
    """Convert GitHub-style > [!TYPE] callouts to HTML divs."""
    lines = text.splitlines()
    out = []
    i = 0
    while i < len(lines):
        line = lines[i]
        m = re.match(r'^>\s*\[!(NOTE|TIP|WARNING)\]\s*$', line.strip())
        if m:
            kind = m.group(1).lower()
            title = kind.capitalize()
            body_lines = []
            i += 1
            while i < len(lines) and lines[i].startswith('>') \
                    and not re.match(r'^>\s*\[!', lines[i]):
                body_lines.append(lines[i][1:].lstrip())
                i += 1
            body = _html.escape('\n'.join(body_lines).strip())
            out.append(
                f'\n<div class="callout callout-{kind}">'
                f'<div class="callout-title">{title}</div>'
                f'<p>{body}</p>'
                f'</div>\n'
            )
        else:
            out.append(line)
            i += 1
    return '\n'.join(out)


def strip_toc(text: str) -> str:
    """Remove the inline TOC list that follows the H1 heading."""
    return re.sub(
        r'(\n# CrossPoint User Guide\n)(.*?)(\n## 1\.)',
        lambda m: m.group(1) + m.group(3),
        text,
        flags=re.DOTALL,
    )


def split_chapters(html: str):
    """Split rendered HTML into (anchor_id, title, html_fragment) tuples by H2."""
    pattern = re.compile(r'(<h2[^>]*>)(.*?)(</h2>)', re.DOTALL)
    chapters = []
    positions = [(m.start(), m.group(0), m.group(2)) for m in pattern.finditer(html)]

    if not positions:
        return [('intro', 'CrossPoint User Guide', html)]

    intro_html = html[:positions[0][0]].strip()
    if intro_html:
        chapters.append(('intro', 'Introduction', intro_html))

    seen_anchors = {c[0] for c in chapters}  # seed with 'intro' if present
    for idx, (start, _tag, raw_title) in enumerate(positions):
        end = positions[idx + 1][0] if idx + 1 < len(positions) else len(html)
        fragment = html[start:end].strip()
        title = _html.unescape(re.sub(r'<[^>]+>', '', raw_title).strip())
        base = re.sub(r'[^a-z0-9]+', '-', title.lower()).strip('-') or 'section'
        anchor = base
        n = 1
        while anchor in seen_anchors:
            anchor = f'{base}-{n}'
            n += 1
        seen_anchors.add(anchor)
        chapters.append((anchor, title, fragment))

    return chapters


def make_xhtml(title: str, body: str) -> bytes:
    return (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!DOCTYPE html PUBLIC "-//W3C//DTD XHTML 1.1//EN" '
        '"http://www.w3.org/TR/xhtml11/DTD/xhtml11.dtd">\n'
        '<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="en">\n'
        f'<head><title>{title}</title>\n'
        '<link rel="stylesheet" type="text/css" href="../style/main.css"/>\n'
        f'</head>\n<body>\n{body}\n</body>\n</html>'
    ).encode('utf-8')


def build_epub():
    source = SOURCE_MD.read_text(encoding='utf-8')
    source = strip_toc(source)
    source = preprocess_callouts(source)

    md = markdown.Markdown(extensions=['tables', 'fenced_code', 'attr_list'])
    body_html = md.convert(source)

    book = epub.EpubBook()
    book.set_identifier('crosspoint-user-guide-v1')
    book.set_title('CrossPoint User Guide')
    book.set_language('en')
    book.add_author('CrossPoint Reader Project')

    style = epub.EpubItem(
        uid='style',
        file_name='style/main.css',
        media_type='text/css',
        content=CSS,
    )
    book.add_item(style)

    # Full-size cover image (480×800) — used by CrossPoint for home-screen thumbnail.
    # uid='cover-image' matches the EPUB 2 <meta name="cover" content="cover-image"/> value.
    cover_png_bytes = make_cover_png()
    cover_img_item = epub.EpubItem(
        uid='cover-image',
        file_name='images/cover.png',
        media_type='image/png',
        content=cover_png_bytes,
    )
    cover_img_item.properties = ['cover-image']  # EPUB 3 manifest property
    book.add_item(cover_img_item)
    # EPUB 2 cover declaration — CrossPoint's Tier 1 OPF lookup
    book.add_metadata('OPF', 'meta', '', {'name': 'cover', 'content': 'cover-image'})

    # Cover page XHTML — first readable page in the spine
    cover_page = epub.EpubHtml(title='CrossPoint User Guide', file_name='cover.xhtml', lang='en')
    cover_page.content = make_xhtml(
        'CrossPoint User Guide',
        '<div class="cover-page">'
        '<img src="images/cover.png" alt="CrossPoint User Guide cover"/>'
        '</div>',
    )
    cover_page.add_item(style)
    cover_page.add_item(cover_img_item)
    book.add_item(cover_page)

    chapters_data = split_chapters(body_html)
    epub_chapters = []

    for anchor, title, fragment in chapters_data:
        filename = f'chap_{anchor}.xhtml'
        chapter = epub.EpubHtml(title=title, file_name=filename, lang='en')
        chapter.content = make_xhtml(title, fragment)
        chapter.add_item(style)
        book.add_item(chapter)
        epub_chapters.append(chapter)

    book.toc = (epub.Link('cover.xhtml', 'Cover', 'cover'),) + tuple(epub_chapters)
    book.add_item(epub.EpubNcx())
    book.add_item(epub.EpubNav())

    # nav is excluded from the spine — CrossPoint locates it via properties="nav" in
    # the manifest and does not respect linear="no", so omitting it prevents it from
    # appearing as a readable page. Cover is the first spine item.
    book.spine = [cover_page] + epub_chapters

    epub.write_epub(str(OUTPUT_EPUB), book)
    print(f'Generated: {OUTPUT_EPUB}')


if __name__ == '__main__':
    build_epub()
