#!/usr/bin/env python3
"""
Generate a text-only EPUB for stress-testing hyphenation, Focus Reading splits,
explicit soft hyphens, slash compounds, and dash compounds.
"""

from __future__ import annotations

import html
import zipfile
from pathlib import Path


OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
OUTPUT_EPUB = OUTPUT_DIR / "test_hyphenation_focus_stress.epub"
TITLE = "Hyphenation and Focus Reading Stress Test"


def soft(word: str) -> str:
    """Replace the ASCII placeholder '|' with the soft-hyphen HTML entity (U+00AD)."""
    return word.replace("|", "&#173;")


def paragraph(ref: str, text: str) -> str:
    return f'<p id="{ref.lower()}"><b>[{ref}]</b> {text}</p>'


def chapter(title: str, slug: str, paragraphs: list[str]) -> tuple[str, str]:
    body = "\n".join(paragraphs)
    xhtml = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head>
  <title>{html.escape(title)}</title>
  <link rel="stylesheet" type="text/css" href="style.css"/>
</head>
<body>
  <h1>{html.escape(title)}</h1>
{body}
</body>
</html>'''
    return f"{slug}.xhtml", xhtml


def make_chapters() -> list[tuple[str, str, str]]:
    chapters: list[tuple[str, str, str]] = []

    chapters.append(
        (
            "1. Baseline Words and Toggle Checks",
            *chapter(
                "1. Baseline Words and Toggle Checks",
                "chapter01-baseline",
                [
                    paragraph(
                        "C1.P01",
                        "This chapter is a control section. Test it with hyphenation on and off, and with Focus "
                        "Reading on and off. Ordinary words should wrap without odd spacing or stranded fragments.",
                    ),
                    paragraph(
                        "C1.P02",
                        "A compact paragraph repeats useful targets: application requirements regulation benefits "
                        "scrutinized probative eligibility overcorrection notorious investigation.",
                    ),
                    paragraph(
                        "C1.P03",
                        "Longer ordinary prose should still feel natural. The investigator described a burdensome "
                        "formal application process, but the surrounding sentences contain no explicit slash or dash "
                        "compounds.",
                    ),
                    paragraph(
                        "C1.P04",
                        "Try Bitter, Noto Serif, OpenDyslexic, and any SD-card fonts. Use this paragraph to compare "
                        "baseline glyph widths before moving to the intentionally awkward chapters.",
                    ),
                ],
            ),
        )
    )

    chapters.append(
        (
            "2. Focus Boundary Hyphenation",
            *chapter(
                "2. Focus Boundary Hyphenation",
                "chapter02-focus-boundary",
                [
                    paragraph(
                        "C2.P01",
                        "The phrase with a good machine learning practice can increase data ingested but decrease "
                        "the burdensome formal application/etc requirements. This repeats the original requirements "
                        "case near several line endings.",
                    ),
                    paragraph(
                        "C2.P02",
                        "Machine learning is not simply useful from a perspective of decreasing fraud. The history "
                        "of regulation of benefits programs is the history of too-late, too-harsh overcorrection to "
                        "notorious abuses.",
                    ),
                    paragraph(
                        "C2.P03",
                        "The contract became a more pay-until-we-decide-to-chase-but-stop-payments-at-that-decision-"
                        "not-after-the-catching arrangement, which should offer pay-ments as a useful boundary split.",
                    ),
                    paragraph(
                        "C2.P04",
                        "Then with my stave I prop my pack upright and sit back against the mountainside, my face in "
                        "cold shade and hot sun on my arms and belly.",
                    ),
                    paragraph(
                        "C2.P05",
                        "Repeat the important words at different offsets: requirements requirements requirements; "
                        "payments payments payments; mountainside mountainside mountainside.",
                    ),
                ],
            ),
        )
    )

    chapters.append(
        (
            "3. Long Dash Compounds",
            *chapter(
                "3. Long Dash Compounds",
                "chapter03-dash-compounds",
                [
                    paragraph(
                        "C3.P01",
                        "The no-time-to-explain-but-please-do-not-break-the-visible-hyphen-chain example should "
                        "allow line breaks after visible hyphens without losing the hyphen.",
                    ),
                    paragraph(
                        "C3.P02",
                        "A fraud-investigation-is-believing-your-log-files-until-they-stop-being-probative phrase "
                        "contains many explicit hyphens and several ordinary hyphenation opportunities.",
                    ),
                    paragraph(
                        "C3.P03",
                        "The committee approved a too-late-too-harsh-overcorrection-to-notorious-abuses policy and "
                        "then wondered why the application-processes-were-voted-on-by-a-subcommittee language was "
                        "hard to read.",
                    ),
                    paragraph(
                        "C3.P04",
                        "This deliberate run tests focus prefixes near hyphens: benefits-programs-eligibility-"
                        "criteria-application-processes-recertification-requirements.",
                    ),
                ],
            ),
        )
    )

    chapters.append(
        (
            "4. Slash Compounds",
            *chapter(
                "4. Slash Compounds",
                "chapter04-slash-compounds",
                [
                    paragraph(
                        "C4.P01",
                        "A burdensome formal application/etc requirements sentence should not create a huge blank "
                        "line when a slash-broken token is followed by a long hyphenatable word.",
                    ),
                    paragraph(
                        "C4.P02",
                        "The reviewer asked about application/eligibility/recertification/requirements and then "
                        "about fraud/identity/password/key-management paperwork.",
                    ),
                    paragraph(
                        "C4.P03",
                        "The interface says benefits/application/processes and regulation/audit/compliance/controls, "
                        "but the reader still needs sane line breaks at narrow widths.",
                    ),
                    paragraph(
                        "C4.P04",
                        "Slash compounds with following punctuation: application/etc, requirements; benefits/"
                        "programs, eligibility; fraud/investigation, probative; mountain/trail, expedition.",
                    ),
                ],
            ),
        )
    )

    chapters.append(
        (
            "5. Explicit Soft Hyphens",
            *chapter(
                "5. Explicit Soft Hyphens",
                "chapter05-soft-hyphens",
                [
                    paragraph(
                        "C5.P01",
                        f"Explicit soft hyphen markers: {soft('re|quire|ments')} should show a visible hyphen only "
                        "when the line actually breaks at one of the inserted soft-hyphen positions.",
                    ),
                    paragraph(
                        "C5.P02",
                        f"More soft hyphen words: {soft('pay|ments')} {soft('moun|tain|side')} "
                        f"{soft('ap|pli|ca|tion')} {soft('over|cor|rec|tion')} {soft('scru|ti|nized')}.",
                    ),
                    paragraph(
                        "C5.P03",
                        f"Soft hyphens inside compounds: pay-until-we-decide-to-stop-{soft('pay|ments')}-at-that-"
                        f"decision and application/etc-{soft('re|quire|ments')} should not disappear incorrectly.",
                    ),
                    paragraph(
                        "C5.P04",
                        f"Soft hyphens around focus boundaries: {soft('moun|tain|side')} {soft('re|quire|ments')} "
                        f"{soft('pay|ments')} repeated in a narrow paragraph to force several attempts.",
                    ),
                ],
            ),
        )
    )

    chapters.append(
        (
            "6. Mixed Stress Paragraphs",
            *chapter(
                "6. Mixed Stress Paragraphs",
                "chapter06-mixed",
                [
                    paragraph(
                        "C6.P01",
                        "A long mixed paragraph: With a good machine-learning practice, an agency can increase "
                        "data ingested but decrease burdensome application/etc requirements, provided the "
                        "fraud-investigation evidence remains probative.",
                    ),
                    paragraph(
                        "C6.P02",
                        "Another mixed paragraph: Pine needles dance in a light breeze against the three white "
                        "sister peaks to the northwest while the expedition-life narrator rests against the "
                        "mountainside.",
                    ),
                    paragraph(
                        "C6.P03",
                        "A final dense run: pay-until-we-decide-to-chase-but-stop-payments-at-that-decision-not-"
                        "after-the-catching application/etc requirements mountainside overcorrection scrutinized "
                        "eligibility.",
                    ),
                    paragraph(
                        "C6.P04",
                        f"Mixed with explicit soft hyphens: {soft('ap|pli|ca|tion')}/etc "
                        f"{soft('re|quire|ments')} and stop-{soft('pay|ments')}-at-that-decision beside "
                        f"{soft('moun|tain|side')}.",
                    ),
                ],
            ),
        )
    )

    return chapters


def create_epub(path: Path) -> None:
    chapters = make_chapters()
    css = """
body {
  line-height: 1.25;
}
h1 {
  margin: 0 0 1em 0;
}
p {
  margin: 0 0 1.15em 0;
  text-align: justify;
}
"""

    manifest_items = [
        '    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>',
        '    <item id="css" href="style.css" media-type="text/css"/>',
    ]
    spine_items = []

    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as epub:
        epub.writestr("mimetype", "application/epub+zip", compress_type=zipfile.ZIP_STORED)
        epub.writestr(
            "META-INF/container.xml",
            """<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>""",
        )
        epub.writestr("OEBPS/style.css", css)

        nav_items = []
        for idx, (chapter_title, chapter_file, chapter_xhtml) in enumerate(chapters, start=1):
            chapter_id = f"chapter{idx:02d}"
            manifest_items.append(
                f'    <item id="{chapter_id}" href="{chapter_file}" media-type="application/xhtml+xml"/>'
            )
            spine_items.append(f'    <itemref idref="{chapter_id}"/>')
            nav_items.append(f'      <li><a href="{chapter_file}">{html.escape(chapter_title)}</a></li>')
            epub.writestr(f"OEBPS/{chapter_file}", chapter_xhtml)

        content_opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">crosspoint-reader-hyphenation-focus-stress</dc:identifier>
    <dc:title>{html.escape(TITLE)}</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
{chr(10).join(manifest_items)}
  </manifest>
  <spine>
{chr(10).join(spine_items)}
  </spine>
</package>'''
        epub.writestr("OEBPS/content.opf", content_opf)

        nav_xhtml = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Navigation</title></head>
<body>
  <nav epub:type="toc">
    <h1>{html.escape(TITLE)}</h1>
    <ol>
{chr(10).join(nav_items)}
    </ol>
  </nav>
</body>
</html>'''
        epub.writestr("OEBPS/nav.xhtml", nav_xhtml)


def main() -> None:
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    create_epub(OUTPUT_EPUB)
    print(f"Wrote {OUTPUT_EPUB}")


if __name__ == "__main__":
    main()
