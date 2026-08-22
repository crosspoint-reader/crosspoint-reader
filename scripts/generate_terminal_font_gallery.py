#!/usr/bin/env python3
"""Render knietty's compiled font tables into a standalone HTML gallery."""

from __future__ import annotations

import argparse
import html
import re
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Font:
    name: str
    profile: str
    source: str
    glyphs: dict[int, tuple[int, ...]]


FONT_SPECS = (
    ("Spleen 8×16", "knietty", "src/terminal/TerminalFontData.generated.h"),
    ("Terminus 8×16", "knietty_terminus", "src/terminal/TerminalFontData.terminus.generated.h"),
    ("GNU Unifont 8×16", "knietty_unifont", "src/terminal/TerminalFontData.unifont.generated.h"),
)

SAMPLES = (
    ("Shell", "(base) user@x4 ~/git/knietty %"),
    ("Legibility", "0O 1Il| []{} ()<> /\\ '‘’\"  .,;:"),
    ("btop / box drawing", "┌──────── CPU ────────┐ │ 42% ███████░░░ │ └─────────────────────┘"),
    ("Blocks and arrows", "← ↑ → ↓  █ ▓ ▒ ░  ▀ ▄ ▌ ▐"),
    ("Powerline", "main  python   12:34   wifi "),
    ("Languages", "café  Ελληνικά  Кириллица"),
)

GLYPH_PATTERN = re.compile(r"\{0x([0-9a-fA-F]{4}), \{([^}]*)\}\}")


def read_font(root: Path, name: str, profile: str, source: str) -> Font:
    glyphs: dict[int, tuple[int, ...]] = {}
    for codepoint, rows in GLYPH_PATTERN.findall((root / source).read_text(encoding="ascii")):
        glyphs[int(codepoint, 16)] = tuple(int(value, 16) for value in re.findall(r"0x([0-9a-fA-F]{2})", rows))
    return Font(name, profile, source, glyphs)


def render_sample(font: Font, sample: str, scale: int = 3) -> tuple[str, int]:
    question = font.glyphs.get(ord("?"), (0,) * 16)
    missing = 0
    pixels: list[str] = []
    for column, character in enumerate(sample):
        rows = font.glyphs.get(ord(character))
        if rows is None:
            rows = question
            missing += 1
        for y, bits in enumerate(rows):
            for x in range(8):
                if bits & (0x80 >> x):
                    pixels.append(
                        f'<rect x="{(column * 10 + 1 + x) * scale}" y="{(1 + y) * scale}" '
                        f'width="{scale}" height="{scale}"/>'
                    )
    width = max(1, len(sample) * 10 * scale)
    svg = (
        f'<svg class="sample" viewBox="0 0 {width} {18 * scale}" role="img" '
        f'aria-label="{html.escape(sample)}"><rect class="paper" width="100%" height="100%"/>'
        + "".join(pixels)
        + "</svg>"
    )
    return svg, missing


def build_page(fonts: list[Font]) -> str:
    cards: list[str] = []
    for font in fonts:
        samples: list[str] = []
        for title, text in SAMPLES:
            svg, missing = render_sample(font, text)
            missing_note = f" · {missing} missing → ?" if missing else ""
            samples.append(
                f'<section><h3>{html.escape(title)}<small>{missing_note}</small></h3>{svg}</section>'
            )
        cards.append(
            f'<article><header><h2>{html.escape(font.name)}</h2>'
            f'<code>pio run -e {html.escape(font.profile)}</code>'
            f'<p>{len(font.glyphs)} compiled glyphs · <code>{html.escape(font.source)}</code></p>'
            f'</header>{"".join(samples)}</article>'
        )
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>knietty terminal font gallery</title>
<style>
:root {{ color-scheme: light dark; font-family: ui-monospace, monospace; }}
body {{ margin: 0 auto; max-width: 1200px; padding: 28px; background: #d8d6cd; color: #191919; }}
h1 {{ margin-bottom: .25rem; }} .intro {{ max-width: 80ch; margin-bottom: 2rem; }}
article {{ background: #f4f1e7; border: 2px solid #222; margin: 0 0 28px; padding: 20px; box-shadow: 5px 5px 0 #777; }}
article header {{ display: flex; align-items: baseline; flex-wrap: wrap; gap: 12px; }}
article header h2 {{ margin: 0; }} article header p {{ width: 100%; margin: 0 0 10px; }}
section {{ margin-top: 18px; }} h3 {{ margin: 0 0 6px; font-size: 14px; }} small {{ font-weight: normal; color: #a22; }}
.sample {{ display: block; width: 100%; min-height: 54px; background: white; border: 1px solid #999; shape-rendering: crispEdges; }}
.sample .paper {{ fill: #fff; }} .sample rect:not(.paper) {{ fill: #000; }} code {{ background: #ddd8cb; padding: 3px 6px; }}
</style></head><body>
<h1>knietty 8×16 terminal fonts</h1>
<p class="intro">These previews are generated directly from the same 1-bit glyph arrays compiled into firmware.
Each glyph is shown unscaled inside knietty's 10×18 cell before the browser magnifies the complete bitmap.
Missing characters use the firmware's literal <code>?</code> fallback.</p>
{"".join(cards)}
</body></html>
"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--output", type=Path, default=Path("docs/terminal-font-gallery.html"))
    args = parser.parse_args()
    fonts = [read_font(args.root, *spec) for spec in FONT_SPECS]
    output = args.output if args.output.is_absolute() else args.root / args.output
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(build_page(fonts), encoding="utf-8")


if __name__ == "__main__":
    main()
