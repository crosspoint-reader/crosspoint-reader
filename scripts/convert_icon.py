import sys
import os
from PIL import Image
import cairosvg
import io

from svg_utils import fit_inside_canvas, parse_svg_intrinsic_size

threshold = 128

def svg_to_png_bytes(svg_path, width, height):
    with open(svg_path, "rb") as f:
        svg_data = f.read()

    src_w, src_h = parse_svg_intrinsic_size(svg_data)
    render_w, render_h = fit_inside_canvas(
        src_w or width, src_h or height, width, height
    )

    png_bytes = cairosvg.svg2png(
        bytestring=svg_data, output_width=render_w, output_height=render_h
    )
    return png_bytes


def center_on_canvas(img, width, height):
    if img.mode != "RGBA":
        img = img.convert("RGBA")
    canvas = Image.new("RGBA", (width, height), (255, 255, 255, 255))
    x = (width - img.width) // 2
    y = (height - img.height) // 2
    canvas.paste(img, (x, y), img)
    return canvas


def load_image(path, width, height):
    ext = os.path.splitext(path)[1].lower()
    if ext == ".svg":
        png_bytes = svg_to_png_bytes(path, width, height)
        img = Image.open(io.BytesIO(png_bytes)).convert("RGBA")
        img = center_on_canvas(img, width, height)
    else:
        img = Image.open(path).convert("RGBA")
        # Keep source aspect ratio and fit inside requested canvas.
        fit = img.copy()
        fit.thumbnail((width, height), Image.LANCZOS)
        img = center_on_canvas(fit, width, height)
        # Flatten alpha: paste on white background
        background = Image.new("RGBA", img.size, (255, 255, 255, 255))
        background.paste(img, mask=img.split()[3])
        img = background
    # Rotate 90 degrees counterclockwise
    img = img.rotate(90, expand=True)
    return img


def image_to_c_array(img, array_name):
    # Convert to grayscale, then threshold to get white=1, black=0
    # Convert to grayscale
    img = img.convert("L")
    width, height = img.size
    pixels = list(img.getdata())
    packed = []
    for y in range(height):
        for x in range(0, width, 8):
            byte = 0
            for b in range(8):
                if x + b < width:
                    v = pixels[y * width + x + b]
                    # 1 for white, 0 for black
                    bit = 1 if v >= threshold else 0
                    byte |= bit << (7 - b)
            packed.append(byte)
    # Format as C array
    c = "#pragma once\n#include <cstdint>\n\n"
    c += f"// size: {width}x{height}\n"
    c += f"static const uint8_t {array_name}[] = {{\n    "
    for i, v in enumerate(packed):
        c += f"0x{v:02X}, "
        if (i + 1) % 16 == 0:
            c += "\n    "
    c = c.rstrip(", \n") + "\n};\n"
    return c


def main():
    if len(sys.argv) < 5:
        print("Usage: python convert_image.py input.png output_name width height")
        sys.exit(1)
    input_path, output_name, width, height = sys.argv[1:5]
    array_name = output_name.capitalize() + "Icon"
    width, height = int(width), int(height)
    img = load_image(input_path, width, height)
    c_array = image_to_c_array(img, array_name)

    # Always save to src/components/icons/[output_name].h relative to project root
    project_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    output_dir = os.path.join(project_root, "src", "components", "icons")
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, f"{output_name}.h")
    with open(output_path, "w") as f:
        f.write(c_array)
    print(f"Wrote {output_path}")


if __name__ == "__main__":
    main()
