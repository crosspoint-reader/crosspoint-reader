"""Convert physical MSB-first 1bpp framebuffers to logical-view PBM images."""


def framebuffer_to_pbm(data, width, height, stride, rotation=0, inverted=False):
    """Rotation is clockwise from physical framebuffer to the upright image."""
    if (
        width <= 0
        or height <= 0
        or stride < (width + 7) // 8
        or len(data) != stride * height
        or rotation not in (0, 90, 180, 270)
    ):
        raise ValueError("Invalid framebuffer geometry or rotation")
    out_width, out_height = (
        (height, width) if rotation in (90, 270) else (width, height)
    )
    out_stride = (out_width + 7) // 8
    pixels = bytearray(out_stride * out_height)
    for py in range(height):
        for px in range(width):
            # Framebuffer 1 means white; PBM 1 means black. Driver inversion
            # changes output polarity without modifying the source framebuffer.
            white = bool(data[py * stride + px // 8] & (0x80 >> (px % 8)))
            if white != inverted:
                continue
            if rotation == 90:
                x, y = height - 1 - py, px
            elif rotation == 180:
                x, y = width - 1 - px, height - 1 - py
            elif rotation == 270:
                x, y = py, width - 1 - px
            else:
                x, y = px, py
            pixels[y * out_stride + x // 8] |= 0x80 >> (x % 8)
    return f"P4\n{out_width} {out_height}\n".encode() + pixels, out_width, out_height
