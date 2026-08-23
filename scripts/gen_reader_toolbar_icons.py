#!/usr/bin/env python3
"""Draws the 24px Contents / Text / More (and the back / next chevrons) glyphs for the toolbar reader menu's
tile row into src/components/icons/readerToolbarIcons.h.

The tile icons are three simple shapes (menu bars, "Aa" in the UI face, an
ellipsis) drawn with Pillow, so no SVG rasterizer is needed -- unlike
freeink-sdk's gen_icons.py, which this mirrors for the packing (1 bpp, MSB
first, 1 = transparent, 0 = ink, plus the optical-centre row).

Run from the repo root:  python scripts/gen_reader_toolbar_icons.py
"""
from PIL import Image, ImageDraw, ImageFont

PX = 24
S = 8  # supersampling factor
FONT = "lib/EpdFont/builtinFonts/source/Ubuntu/Ubuntu-Medium.ttf"
OUT = "src/components/icons/readerToolbarIcons.h"


def canvas():
    return Image.new("L", (PX * S, PX * S), 255)


def down(im):
    return im.resize((PX, PX), Image.LANCZOS)


def draw_icons():
    icons = {}
    im = canvas()
    d = ImageDraw.Draw(im)
    for y in (6, 12, 18):
        d.line([(4 * S, y * S), (20 * S, y * S)], fill=0, width=2 * S)
    icons["contents"] = down(im)

    im = canvas()
    d = ImageDraw.Draw(im)
    f = ImageFont.truetype(FONT, 19 * S)
    bb = d.textbbox((0, 0), "Aa", font=f)
    w, h = bb[2] - bb[0], bb[3] - bb[1]
    d.text(((PX * S - w) // 2 - bb[0], (PX * S - h) // 2 - bb[1]), "Aa", font=f, fill=0)
    icons["text"] = down(im)

    im = canvas()
    d = ImageDraw.Draw(im)
    for x in (5, 12, 19):
        r = 1.6 * S
        d.ellipse([(x * S - r, 12 * S - r), (x * S + r, 12 * S + r)], fill=0)
    icons["more"] = down(im)

    im = canvas()
    d = ImageDraw.Draw(im)
    d.line([(15 * S, 4 * S), (8 * S, 12 * S), (15 * S, 20 * S)], fill=0, width=2 * S, joint="curve")
    icons["back"] = down(im)

    im = canvas()
    d = ImageDraw.Draw(im)
    d.line([(9 * S, 4 * S), (16 * S, 12 * S), (9 * S, 20 * S)], fill=0, width=2 * S, joint="curve")
    icons["next"] = down(im)
    return icons


def pack(img):
    pix = img.load()
    data = []
    sum_y = 0
    count = 0
    for y in range(PX):
        for xb in range(0, PX, 8):
            byte = 0
            for k in range(8):
                x = xb + k
                white = 1
                if pix[x, y] < 128:
                    white = 0
                    sum_y += y
                    count += 1
                byte |= white << (7 - k)
            data.append(byte)
    return data, round(sum_y / count) if count else PX // 2


def main():
    out = [
        "#pragma once",
        "",
        '#include "Icon.h"',
        "",
        "// Glyphs for the toolbar reader menu's Contents / Text / More tiles (24px,",
        "// 1 = transparent, 0 = ink, MSB first -- the gen_icons.py packing). Drawn",
        "// with scripts/gen_reader_toolbar_icons.py; rerun it to change them.",
        "",
    ]
    for name, im in draw_icons().items():
        data, c = pack(im)
        body = ", ".join(f"0x{b:02X}" for b in data)
        out.append(f"// {name}")
        out.append(f"static constexpr uint8_t icon_reader_{name}_24_bits[] = {{{body}}};")
        out.append(f"static constexpr freeink::Icon icon_reader_{name}_24 = {{{PX}, {PX}, {c}, icon_reader_{name}_24_bits}};")
        out.append("")
    open(OUT, "w", newline="\n").write("\n".join(out))
    print(f"wrote {OUT}")


if __name__ == "__main__":
    main()
