#!/usr/bin/env python3
"""
Generate lib/Weather/WeatherIcons48.h from SVG sources.

By default, this script loads SVG files from assets/weather-icons/svg.
Use --fetch to download missing files from erikflowers/weather-icons.
"""

import argparse
import io
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import urllib.request
import zipfile

from PIL import Image

from svg_utils import fit_inside_canvas, parse_svg_intrinsic_size

try:
    import cairosvg  # type: ignore
except ImportError:
    cairosvg = None


SIZE = 64
# Higher threshold slightly thickens dark icon strokes after antialiasing.
THRESHOLD = 160
# Keep zero margin so rendered glyphs can use the full 64x64 canvas.
CONTENT_MARGIN = 0
PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_SVG_DIR = PROJECT_ROOT / "assets" / "weather-icons" / "svg"
DEFAULT_OUT = PROJECT_ROOT / "lib" / "Weather" / "WeatherIconsLarge.h"
UPSTREAM_BASE = "https://raw.githubusercontent.com/erikflowers/weather-icons/master/svg"
RESVG_ZIP_URL = (
    "https://github.com/linebender/resvg/releases/latest/download/resvg-win64.zip"
)
RESVG_EXE = PROJECT_ROOT / ".cache" / "resvg" / "resvg.exe"

ICON_SOURCES = {
    "WI_LARGE_CLEAR_DAY": "wi-day-sunny.svg",
    "WI_LARGE_CLEAR_NIGHT": "wi-night-clear.svg",
    "WI_LARGE_PARTLY_CLOUDY_DAY": "wi-day-cloudy.svg",
    "WI_LARGE_PARTLY_CLOUDY_NIGHT": "wi-night-alt-cloudy.svg",
    "WI_LARGE_OVERCAST": "wi-cloudy.svg",
    "WI_LARGE_FOG": "wi-fog.svg",
    "WI_LARGE_DRIZZLE": "wi-sprinkle.svg",
    "WI_LARGE_RAIN": "wi-rain.svg",
    "WI_LARGE_SNOW": "wi-snow.svg",
    "WI_LARGE_THUNDERSTORM": "wi-thunderstorm.svg",
}


def ensure_resvg_binary():
    if shutil.which("resvg"):
        return Path(shutil.which("resvg"))

    if RESVG_EXE.exists():
        return RESVG_EXE

    RESVG_EXE.parent.mkdir(parents=True, exist_ok=True)
    archive_path = RESVG_EXE.parent / "resvg.zip"
    with urllib.request.urlopen(RESVG_ZIP_URL) as response:
        archive_path.write_bytes(response.read())

    with zipfile.ZipFile(archive_path, "r") as zf:
        zf.extractall(RESVG_EXE.parent)

    found = list(RESVG_EXE.parent.rglob("resvg.exe"))
    if not found:
        raise RuntimeError("resvg.exe not found after extraction")

    if found[0] != RESVG_EXE:
        RESVG_EXE.write_bytes(found[0].read_bytes())

    return RESVG_EXE


def render_svg_with_resvg(svg_data, render_w, render_h):
    exe = ensure_resvg_binary()
    with tempfile.TemporaryDirectory() as tmp:
        tmp_path = Path(tmp)
        in_svg = tmp_path / "icon.svg"
        out_png = tmp_path / "icon.png"
        in_svg.write_bytes(svg_data)

        cmd = [
            str(exe),
            "--width",
            str(render_w),
            "--height",
            str(render_h),
            str(in_svg),
            str(out_png),
        ]
        subprocess.run(cmd, check=True, capture_output=True)
        return out_png.read_bytes()


def render_svg_contain(svg_data, width, height):
    src_w, src_h = parse_svg_intrinsic_size(svg_data)
    render_w, render_h = fit_inside_canvas(
        src_w or width, src_h or height, width, height
    )

    # Render larger first so trimming and re-fit preserve detail quality.
    oversample = 4
    render_w *= oversample
    render_h *= oversample

    if cairosvg is not None:
        png_bytes = cairosvg.svg2png(
            bytestring=svg_data, output_width=render_w, output_height=render_h
        )
    else:
        png_bytes = render_svg_with_resvg(svg_data, render_w, render_h)

    icon = Image.open(io.BytesIO(png_bytes)).convert("RGBA")

    # Trim transparent/empty margins so symbols use available icon area better.
    alpha_bbox = icon.split()[3].getbbox()
    if alpha_bbox is not None:
        icon = icon.crop(alpha_bbox)

    max_w = max(1, width - 2 * CONTENT_MARGIN)
    max_h = max(1, height - 2 * CONTENT_MARGIN)
    icon.thumbnail((max_w, max_h), Image.Resampling.LANCZOS)

    canvas = Image.new("RGBA", (width, height), (255, 255, 255, 255))
    off_x = (width - icon.width) // 2
    off_y = (height - icon.height) // 2
    canvas.paste(icon, (off_x, off_y), icon)

    # Flatten alpha on white and convert to monochrome-friendly grayscale.
    flat = Image.new("RGBA", canvas.size, (255, 255, 255, 255))
    flat.paste(canvas, mask=canvas.split()[3])
    return flat.convert("L")


def image_to_packed_bits(img):
    width, height = img.size
    pixels = img.tobytes()
    packed = []
    for y in range(height):
        for x in range(0, width, 8):
            out = 0
            for b in range(8):
                px = x + b
                lum = pixels[y * width + px] if px < width else 255
                # 1-bit means white/clear (not drawn), 0-bit means black/drawn.
                bit = 1 if lum >= THRESHOLD else 0
                out |= bit << (7 - b)
            packed.append(out)
    return packed


def format_array(name, data):
    lines = []
    per_line = 16
    for i in range(0, len(data), per_line):
        chunk = data[i : i + per_line]
        lines.append("  " + ", ".join(f"0x{v:02X}" for v in chunk) + ",")
    body = "\n".join(lines)
    return f"static const uint8_t {name}[] = {{\n{body}\n}};\n"


def ensure_svg(path, fetch):
    if path.exists():
        return
    if not fetch:
        raise FileNotFoundError(f"Missing SVG: {path}")

    path.parent.mkdir(parents=True, exist_ok=True)
    url = f"{UPSTREAM_BASE}/{path.name}"
    with urllib.request.urlopen(url) as response:
        data = response.read()
    path.write_bytes(data)


def generate(svg_dir, output_path, fetch):
    arrays = []
    for symbol, filename in ICON_SOURCES.items():
        svg_path = svg_dir / filename
        ensure_svg(svg_path, fetch)
        svg_data = svg_path.read_bytes()
        img = render_svg_contain(svg_data, SIZE, SIZE)
        packed = image_to_packed_bits(img)
        arrays.append(format_array(symbol, packed))

    header = [
        "#pragma once",
        "#include <cstdint>",
        "",
        "// Generated from erikflowers/weather-icons SVGs.",
        f"// {SIZE}x{SIZE}, 1-bit, MSB-first, row-major.",
        "// Regenerate with: python scripts/generate_weather_icons.py --fetch",
        "// clang-format off",
        f"constexpr int WEATHER_ICON_SIZE = {SIZE};  // Large icons for weather",
        "",
    ]
    header.extend(arrays)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(header) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(
        description="Generate WeatherIconsLarge.h from SVG files"
    )
    parser.add_argument(
        "--svg-dir",
        type=Path,
        default=DEFAULT_SVG_DIR,
        help="Directory containing source SVG files",
    )
    parser.add_argument(
        "--output", type=Path, default=DEFAULT_OUT, help="Output header path"
    )
    parser.add_argument(
        "--fetch", action="store_true", help="Fetch missing SVG files from upstream"
    )
    args = parser.parse_args()

    generate(args.svg_dir, args.output, args.fetch)
    rel_out = os.path.relpath(args.output, PROJECT_ROOT)
    print(f"Wrote {rel_out}")


if __name__ == "__main__":
    main()
