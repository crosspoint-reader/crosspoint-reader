#!/usr/bin/env python3
"""Generate the deterministic font-prewarm benchmark EPUB."""

from pathlib import Path
from zipfile import ZIP_DEFLATED, ZIP_STORED, ZipFile, ZipInfo


OUTPUT = Path(__file__).with_name("font-prewarm-benchmark.epub")
FIXED_TIME = (2026, 1, 1, 0, 0, 0)
SAMPLE_COUNT = 240


def xhtml_page(title: str, body: str) -> str:
    return f"""<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" lang="en">
<head>
  <meta charset="UTF-8"/>
  <title>{title}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
{body}
</body>
</html>
"""


def sample_chapter(mixed_styles: bool) -> str:
    regular = "abcdefghijklm abcdefghijklm "
    bold = "nopqrstuvwxyz nopqrstuvwxyz "
    italic = "ABCDEFGHIJKLM ABCDEFGHIJKLM "
    bold_italic = "NOPQRSTUVWXYZ NOPQRSTUVWXYZ"

    if mixed_styles:
        sample = (
            f'{regular}<strong>{bold}</strong><em>{italic}</em>'
            f'<strong><em>{bold_italic}</em></strong>'
        )
    else:
        sample = regular + bold + italic + bold_italic

    rows = "\n".join(f'<p class="sample">{sample}</p>' for _ in range(SAMPLE_COUNT))
    title = "Mixed styles" if mixed_styles else "Regular-only control"
    return xhtml_page(title, f"<h1>{title}</h1>\n{rows}")


FILES = {
    "META-INF/container.xml": """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>
""",
    "OEBPS/content.opf": """<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="book-id">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="book-id">crosspoint-font-prewarm-benchmark</dc:identifier>
    <dc:title>CrossPoint Font Prewarm Benchmark</dc:title>
    <dc:language>en</dc:language>
    <meta property="dcterms:modified">2026-01-01T00:00:00Z</meta>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
    <item id="css" href="style.css" media-type="text/css"/>
    <item id="instructions" href="instructions.xhtml" media-type="application/xhtml+xml"/>
    <item id="mixed" href="mixed.xhtml" media-type="application/xhtml+xml"/>
    <item id="control" href="control.xhtml" media-type="application/xhtml+xml"/>
  </manifest>
  <spine>
    <itemref idref="instructions"/>
    <itemref idref="mixed"/>
    <itemref idref="control"/>
  </spine>
</package>
""",
    "OEBPS/nav.xhtml": xhtml_page(
        "Contents",
        """<nav epub:type="toc" xmlns:epub="http://www.idpf.org/2007/ops">
  <h1>Contents</h1>
  <ol>
    <li><a href="instructions.xhtml">Instructions</a></li>
    <li><a href="mixed.xhtml">Mixed styles</a></li>
    <li><a href="control.xhtml">Regular-only control</a></li>
  </ol>
</nav>""",
    ),
    "OEBPS/style.css": """body { margin: 0; padding: 0; }
h1 { break-after: page; }
.sample { margin: 0; padding: 0; text-indent: 0; }
""",
    "OEBPS/instructions.xhtml": xhtml_page(
        "Instructions",
        """<h1>Font prewarm benchmark</h1>
<p>This book compares a mixed regular, bold, italic, and bold-italic workload with a regular-only control containing exactly the same characters.</p>
<ol>
  <li>Use the same device, firmware settings, orientation, font size, antialiasing, and refresh mode for both builds.</li>
  <li>Test once with a built-in four-style family and once with the SD-card Literata family.</li>
  <li>Open Mixed styles, discard the first rendered page, then record ten sequential ERS Page render log lines.</li>
  <li>Repeat from the first page of Regular-only control.</li>
  <li>Record the periodic MEM Free, Min Free, and MaxAlloc line before the run and after repeated page turns.</li>
</ol>
<p>The mixed chapter isolates style-aware prewarming. The regular-only chapter is a control for scanner and display costs. Repeating the run exposes transient heap allocation and fragmentation.</p>""",
    ),
    "OEBPS/mixed.xhtml": sample_chapter(True),
    "OEBPS/control.xhtml": sample_chapter(False),
}


def add_file(archive: ZipFile, name: str, contents: str, compression: int) -> None:
    info = ZipInfo(name, FIXED_TIME)
    info.compress_type = compression
    info.external_attr = 0o100644 << 16
    archive.writestr(info, contents.encode("utf-8"))


def main() -> None:
    with ZipFile(OUTPUT, "w") as archive:
        add_file(archive, "mimetype", "application/epub+zip", ZIP_STORED)
        for name, contents in FILES.items():
            add_file(archive, name, contents, ZIP_DEFLATED)


if __name__ == "__main__":
    main()
