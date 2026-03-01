#!/usr/bin/env python3
"""
Generate test EPUBs for image rendering verification.

Creates EPUBs with annotated JPEG and PNG images to verify:
- Grayscale rendering (4 levels)
- Image scaling
- Image centering
- Cache performance
- Page serialization
"""

import os
import zipfile
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Please install Pillow: pip install Pillow")
    exit(1)

OUTPUT_DIR = Path(__file__).parent.parent / "test" / "epubs"
SCREEN_WIDTH = 480
SCREEN_HEIGHT = 800

def get_font(size=20):
    """Get a font, falling back to default if needed."""
    try:
        return ImageFont.truetype("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", size)
    except:
        try:
            return ImageFont.truetype("/usr/share/fonts/TTF/DejaVuSans.ttf", size)
        except:
            return ImageFont.load_default()

def draw_text_centered(draw, y, text, font, fill=0):
    """Draw centered text at given y position."""
    bbox = draw.textbbox((0, 0), text, font=font)
    text_width = bbox[2] - bbox[0]
    x = (draw.im.size[0] - text_width) // 2
    draw.text((x, y), text, font=font, fill=fill)

def draw_text_wrapped(draw, x, y, text, font, max_width, fill=0):
    """Draw text with word wrapping."""
    words = text.split()
    lines = []
    current_line = []

    for word in words:
        test_line = ' '.join(current_line + [word])
        bbox = draw.textbbox((0, 0), test_line, font=font)
        if bbox[2] - bbox[0] <= max_width:
            current_line.append(word)
        else:
            if current_line:
                lines.append(' '.join(current_line))
            current_line = [word]
    if current_line:
        lines.append(' '.join(current_line))

    line_height = font.size + 4 if hasattr(font, 'size') else 20
    for i, line in enumerate(lines):
        draw.text((x, y + i * line_height), line, font=font, fill=fill)

    return len(lines) * line_height

def create_grayscale_test_image(filename, is_png=True):
    """
    Create image with 4 grayscale squares to verify 4-level rendering.
    """
    width, height = 400, 600
    img = Image.new('L', (width, height), 255)
    draw = ImageDraw.Draw(img)
    font = get_font(16)
    font_small = get_font(14)

    # Title
    draw_text_centered(draw, 10, "GRAYSCALE TEST", font, fill=0)
    draw_text_centered(draw, 35, "Verify 4 distinct gray levels", font_small, fill=64)

    # Draw 4 grayscale squares
    square_size = 70
    start_y = 65
    gap = 10

    # Gray levels chosen to avoid Bayer dithering threshold boundaries (±40 dither offset)
    # Thresholds at 64, 128, 192 - use values in the middle of each band for solid output
    # Safe zones: 0-23 (black), 88-103 (dark gray), 152-167 (light gray), 232-255 (white)
    levels = [
        (0, "Level 0: BLACK"),
        (96, "Level 1: DARK GRAY"),
        (160, "Level 2: LIGHT GRAY"),
        (255, "Level 3: WHITE"),
    ]

    for i, (gray_value, label) in enumerate(levels):
        y = start_y + i * (square_size + gap + 22)
        x = (width - square_size) // 2

        # Draw square with border
        draw.rectangle([x-2, y-2, x + square_size + 2, y + square_size + 2], fill=0)
        draw.rectangle([x, y, x + square_size, y + square_size], fill=gray_value)

        # Label below square
        bbox = draw.textbbox((0, 0), label, font=font_small)
        label_width = bbox[2] - bbox[0]
        draw.text(((width - label_width) // 2, y + square_size + 5), label, font=font_small, fill=0)

    # Instructions at bottom (well below the last square)
    y = height - 70
    draw_text_centered(draw, y, "PASS: 4 distinct shades visible", font_small, fill=0)
    draw_text_centered(draw, y + 20, "FAIL: Only black/white or", font_small, fill=64)
    draw_text_centered(draw, y + 38, "muddy/indistinct grays", font_small, fill=64)

    # Save
    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_centering_test_image(filename, is_png=True):
    """
    Create image with border markers to verify centering.
    """
    width, height = 350, 400
    img = Image.new('L', (width, height), 255)
    draw = ImageDraw.Draw(img)
    font = get_font(16)
    font_small = get_font(14)

    # Draw border
    draw.rectangle([0, 0, width-1, height-1], outline=0, width=3)

    # Corner markers
    marker_size = 20
    for x, y in [(0, 0), (width-marker_size, 0), (0, height-marker_size), (width-marker_size, height-marker_size)]:
        draw.rectangle([x, y, x+marker_size, y+marker_size], fill=0)

    # Center cross
    cx, cy = width // 2, height // 2
    draw.line([cx - 30, cy, cx + 30, cy], fill=0, width=2)
    draw.line([cx, cy - 30, cx, cy + 30], fill=0, width=2)

    # Title
    draw_text_centered(draw, 40, "CENTERING TEST", font, fill=0)

    # Instructions
    y = 80
    draw_text_centered(draw, y, "Image should be centered", font_small, fill=0)
    draw_text_centered(draw, y + 20, "horizontally on screen", font_small, fill=0)

    y = 150
    draw_text_centered(draw, y, "Check:", font_small, fill=0)
    draw_text_centered(draw, y + 25, "- Equal margins left & right", font_small, fill=64)
    draw_text_centered(draw, y + 45, "- All 4 corners visible", font_small, fill=64)
    draw_text_centered(draw, y + 65, "- Border is complete rectangle", font_small, fill=64)

    # Pass/fail
    y = height - 80
    draw_text_centered(draw, y, "PASS: Centered, all corners visible", font_small, fill=0)
    draw_text_centered(draw, y + 20, "FAIL: Off-center or cropped", font_small, fill=64)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_scaling_test_image(filename, is_png=True):
    """
    Create large image to verify scaling works.
    """
    # Make image larger than screen but within decoder limits (max 2048x1536)
    width, height = 1200, 1500
    img = Image.new('L', (width, height), 240)
    draw = ImageDraw.Draw(img)
    font = get_font(48)
    font_medium = get_font(32)
    font_small = get_font(24)

    # Border
    draw.rectangle([0, 0, width-1, height-1], outline=0, width=8)
    draw.rectangle([20, 20, width-21, height-21], outline=128, width=4)

    # Title
    draw_text_centered(draw, 60, "SCALING TEST", font, fill=0)
    draw_text_centered(draw, 130, f"Original: {width}x{height} (larger than screen)", font_medium, fill=64)

    # Grid pattern to verify scaling quality
    grid_start_y = 220
    grid_size = 400
    cell_size = 50

    draw_text_centered(draw, grid_start_y - 40, "Grid pattern (check for artifacts):", font_small, fill=0)

    grid_x = (width - grid_size) // 2
    for row in range(grid_size // cell_size):
        for col in range(grid_size // cell_size):
            x = grid_x + col * cell_size
            y = grid_start_y + row * cell_size
            if (row + col) % 2 == 0:
                draw.rectangle([x, y, x + cell_size, y + cell_size], fill=0)
            else:
                draw.rectangle([x, y, x + cell_size, y + cell_size], fill=200)

    # Size indicator bars
    y = grid_start_y + grid_size + 60
    draw_text_centered(draw, y, "Width markers (should fit on screen):", font_small, fill=0)

    bar_y = y + 40
    # Full width bar
    draw.rectangle([50, bar_y, width - 50, bar_y + 30], fill=0)
    draw.text((60, bar_y + 5), "FULL WIDTH", font=font_small, fill=255)

    # Half width bar
    bar_y += 60
    half_start = width // 4
    draw.rectangle([half_start, bar_y, width - half_start, bar_y + 30], fill=85)
    draw.text((half_start + 10, bar_y + 5), "HALF WIDTH", font=font_small, fill=255)

    # Instructions
    y = height - 350
    draw_text_centered(draw, y, "VERIFICATION:", font_medium, fill=0)
    y += 50
    instructions = [
        "1. Image fits within screen bounds",
        "2. All borders visible (not cropped)",
        "3. Grid pattern clear (no moire)",
        "4. Text readable after scaling",
        "5. Aspect ratio preserved (not stretched)",
    ]
    for i, text in enumerate(instructions):
        draw_text_centered(draw, y + i * 35, text, font_small, fill=64)

    y = height - 100
    draw_text_centered(draw, y, "PASS: Scaled down, readable, complete", font_small, fill=0)
    draw_text_centered(draw, y + 30, "FAIL: Cropped, distorted, or unreadable", font_small, fill=64)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_wide_scaling_test_image(filename, is_png=True):
    """
    Create wide image (1807x736) to test scaling with specific dimensions
    that can trigger cache dimension mismatches due to floating-point rounding.
    """
    width, height = 1807, 736
    img = Image.new('L', (width, height), 240)
    draw = ImageDraw.Draw(img)
    font = get_font(48)
    font_medium = get_font(32)
    font_small = get_font(24)

    # Border
    draw.rectangle([0, 0, width-1, height-1], outline=0, width=6)
    draw.rectangle([15, 15, width-16, height-16], outline=128, width=3)

    # Title
    draw_text_centered(draw, 40, "WIDE SCALING TEST", font, fill=0)
    draw_text_centered(draw, 100, f"Original: {width}x{height} (tests rounding edge case)", font_medium, fill=64)

    # Grid pattern to verify scaling quality
    grid_start_x = 100
    grid_start_y = 180
    grid_width = 600
    grid_height = 300
    cell_size = 50

    draw.text((grid_start_x, grid_start_y - 35), "Grid pattern (check for artifacts):", font=font_small, fill=0)

    for row in range(grid_height // cell_size):
        for col in range(grid_width // cell_size):
            x = grid_start_x + col * cell_size
            y = grid_start_y + row * cell_size
            if (row + col) % 2 == 0:
                draw.rectangle([x, y, x + cell_size, y + cell_size], fill=0)
            else:
                draw.rectangle([x, y, x + cell_size, y + cell_size], fill=200)

    # Verification section on the right
    text_x = 800
    text_y = 180
    draw.text((text_x, text_y), "VERIFICATION:", font=font_medium, fill=0)
    text_y += 50
    instructions = [
        "1. Image fits within screen",
        "2. All borders visible",
        "3. Grid pattern clear",
        "4. Text readable",
        "5. No double-decode in log",
    ]
    for i, text in enumerate(instructions):
        draw.text((text_x, text_y + i * 35), text, font=font_small, fill=64)

    # Dimension info
    draw.text((text_x, 450), f"Dimensions: {width}x{height}", font=font_small, fill=0)
    draw.text((text_x, 485), "Tests cache dimension matching", font=font_small, fill=64)

    # Pass/fail at bottom
    y = height - 80
    draw_text_centered(draw, y, "PASS: Single decode, cached correctly", font_small, fill=0)
    draw_text_centered(draw, y + 30, "FAIL: Cache mismatch, multiple decodes", font_small, fill=64)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_cache_test_image(filename, page_num, is_png=True):
    """
    Create image for cache performance testing.
    """
    width, height = 400, 300
    img = Image.new('L', (width, height), 255)
    draw = ImageDraw.Draw(img)
    font = get_font(18)
    font_small = get_font(14)
    font_large = get_font(36)

    # Border
    draw.rectangle([0, 0, width-1, height-1], outline=0, width=2)

    # Page number prominent
    draw_text_centered(draw, 30, f"CACHE TEST PAGE {page_num}", font, fill=0)
    draw_text_centered(draw, 80, f"#{page_num}", font_large, fill=0)

    # Instructions
    y = 140
    draw_text_centered(draw, y, "Navigate away then return", font_small, fill=64)
    draw_text_centered(draw, y + 25, "Second load should be faster", font_small, fill=64)

    y = 220
    draw_text_centered(draw, y, "PASS: Faster reload from cache", font_small, fill=0)
    draw_text_centered(draw, y + 20, "FAIL: Same slow decode each time", font_small, fill=64)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_gradient_test_image(filename, is_png=True):
    """
    Create horizontal gradient to test grayscale banding.
    """
    width, height = 400, 500
    img = Image.new('L', (width, height), 255)
    draw = ImageDraw.Draw(img)
    font = get_font(16)
    font_small = get_font(14)

    draw_text_centered(draw, 10, "GRADIENT TEST", font, fill=0)
    draw_text_centered(draw, 35, "Smooth gradient → 4 bands expected", font_small, fill=64)

    # Horizontal gradient
    gradient_y = 70
    gradient_height = 100
    for x in range(width):
        gray = int(255 * x / width)
        draw.line([(x, gradient_y), (x, gradient_y + gradient_height)], fill=gray)

    # Border around gradient
    draw.rectangle([0, gradient_y-1, width-1, gradient_y + gradient_height + 1], outline=0, width=1)

    # Labels
    y = gradient_y + gradient_height + 10
    draw.text((5, y), "BLACK", font=font_small, fill=0)
    draw.text((width - 50, y), "WHITE", font=font_small, fill=0)

    # 4-step gradient (what it should look like)
    y = 220
    draw_text_centered(draw, y, "Expected result (4 distinct bands):", font_small, fill=0)

    band_y = y + 25
    band_height = 60
    band_width = width // 4
    for i, gray in enumerate([0, 85, 170, 255]):
        x = i * band_width
        draw.rectangle([x, band_y, x + band_width, band_y + band_height], fill=gray)
    draw.rectangle([0, band_y-1, width-1, band_y + band_height + 1], outline=0, width=1)

    # Vertical gradient
    y = 340
    draw_text_centered(draw, y, "Vertical gradient:", font_small, fill=0)

    vgrad_y = y + 25
    vgrad_height = 80
    for row in range(vgrad_height):
        gray = int(255 * row / vgrad_height)
        draw.line([(50, vgrad_y + row), (width - 50, vgrad_y + row)], fill=gray)
    draw.rectangle([49, vgrad_y-1, width-49, vgrad_y + vgrad_height + 1], outline=0, width=1)

    # Pass/fail
    y = height - 50
    draw_text_centered(draw, y, "PASS: Clear 4-band quantization", font_small, fill=0)
    draw_text_centered(draw, y + 20, "FAIL: Binary/noisy dithering", font_small, fill=64)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_format_test_image(filename, format_name, is_png=True):
    """
    Create simple image to verify format support.
    """
    width, height = 350, 250
    img = Image.new('L', (width, height), 255)
    draw = ImageDraw.Draw(img)
    font = get_font(20)
    font_large = get_font(36)
    font_small = get_font(14)

    # Border
    draw.rectangle([0, 0, width-1, height-1], outline=0, width=3)

    # Format name
    draw_text_centered(draw, 30, f"{format_name} FORMAT TEST", font, fill=0)
    draw_text_centered(draw, 80, format_name, font_large, fill=0)

    # Checkmark area
    y = 140
    draw_text_centered(draw, y, "If you can read this,", font_small, fill=64)
    draw_text_centered(draw, y + 20, f"{format_name} decoding works!", font_small, fill=64)

    y = height - 40
    draw_text_centered(draw, y, f"PASS: {format_name} image visible", font_small, fill=0)

    if is_png:
        img.save(filename, 'PNG')
    else:
        img.save(filename, 'JPEG', quality=95)

def create_epub(epub_path, title, chapters):
    """
    Create an EPUB file with the given chapters.

    chapters: list of (chapter_title, html_content, images)
              images: list of (image_filename, image_data)
    """
    with zipfile.ZipFile(epub_path, 'w', zipfile.ZIP_DEFLATED) as epub:
        # mimetype (must be first, uncompressed)
        epub.writestr('mimetype', 'application/epub+zip', compress_type=zipfile.ZIP_STORED)

        # Container
        container_xml = '''<?xml version="1.0" encoding="UTF-8"?>
<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">
  <rootfiles>
    <rootfile full-path="OEBPS/content.opf" media-type="application/oebps-package+xml"/>
  </rootfiles>
</container>'''
        epub.writestr('META-INF/container.xml', container_xml)

        # Collect all images and chapters
        manifest_items = []
        spine_items = []

        # Add chapters and images
        for i, (chapter_title, html_content, images) in enumerate(chapters):
            chapter_id = f'chapter{i+1}'
            chapter_file = f'chapter{i+1}.xhtml'

            # Add images for this chapter
            for img_filename, img_data in images:
                media_type = 'image/png' if img_filename.endswith('.png') else 'image/jpeg'
                manifest_items.append(f'    <item id="{img_filename.replace(".", "_")}" href="images/{img_filename}" media-type="{media_type}"/>')
                epub.writestr(f'OEBPS/images/{img_filename}', img_data)

            # Add chapter
            manifest_items.append(f'    <item id="{chapter_id}" href="{chapter_file}" media-type="application/xhtml+xml"/>')
            spine_items.append(f'    <itemref idref="{chapter_id}"/>')
            epub.writestr(f'OEBPS/{chapter_file}', html_content)

        # content.opf
        content_opf = f'''<?xml version="1.0" encoding="UTF-8"?>
<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="uid">
  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">
    <dc:identifier id="uid">test-epub-{title.lower().replace(" ", "-")}</dc:identifier>
    <dc:title>{title}</dc:title>
    <dc:language>en</dc:language>
  </metadata>
  <manifest>
    <item id="nav" href="nav.xhtml" media-type="application/xhtml+xml" properties="nav"/>
{chr(10).join(manifest_items)}
  </manifest>
  <spine>
{chr(10).join(spine_items)}
  </spine>
</package>'''
        epub.writestr('OEBPS/content.opf', content_opf)

        # Navigation document
        nav_items = '\n'.join([f'      <li><a href="chapter{i+1}.xhtml">{chapters[i][0]}</a></li>'
                               for i in range(len(chapters))])
        nav_xhtml = f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml" xmlns:epub="http://www.idpf.org/2007/ops">
<head><title>Navigation</title></head>
<body>
  <nav epub:type="toc">
    <h1>Contents</h1>
    <ol>
{nav_items}
    </ol>
  </nav>
</body>
</html>'''
        epub.writestr('OEBPS/nav.xhtml', nav_xhtml)

def make_chapter(title, body_content):
    """Create XHTML chapter content."""
    return f'''<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE html>
<html xmlns="http://www.w3.org/1999/xhtml">
<head><title>{title}</title></head>
<body>
<h1>{title}</h1>
{body_content}
</body>
</html>'''

def main():
    OUTPUT_DIR.mkdir(exist_ok=True)

    # Temp directory for images
    import tempfile
    with tempfile.TemporaryDirectory() as tmpdir:
        tmpdir = Path(tmpdir)

        print("Generating test images...")

        # Generate all test images
        images = {}

        # JPEG tests
        create_grayscale_test_image(tmpdir / 'grayscale_test.jpg', is_png=False)
        create_centering_test_image(tmpdir / 'centering_test.jpg', is_png=False)
        create_scaling_test_image(tmpdir / 'scaling_test.jpg', is_png=False)
        create_wide_scaling_test_image(tmpdir / 'wide_scaling_test.jpg', is_png=False)
        create_gradient_test_image(tmpdir / 'gradient_test.jpg', is_png=False)
        create_format_test_image(tmpdir / 'jpeg_format.jpg', 'JPEG', is_png=False)
        create_cache_test_image(tmpdir / 'cache_test_1.jpg', 1, is_png=False)
        create_cache_test_image(tmpdir / 'cache_test_2.jpg', 2, is_png=False)

        # PNG tests
        create_grayscale_test_image(tmpdir / 'grayscale_test.png', is_png=True)
        create_centering_test_image(tmpdir / 'centering_test.png', is_png=True)
        create_scaling_test_image(tmpdir / 'scaling_test.png', is_png=True)
        create_wide_scaling_test_image(tmpdir / 'wide_scaling_test.png', is_png=True)
        create_gradient_test_image(tmpdir / 'gradient_test.png', is_png=True)
        create_format_test_image(tmpdir / 'png_format.png', 'PNG', is_png=True)
        create_cache_test_image(tmpdir / 'cache_test_1.png', 1, is_png=True)
        create_cache_test_image(tmpdir / 'cache_test_2.png', 2, is_png=True)

        # Read all images
        for img_file in tmpdir.glob('*.*'):
            images[img_file.name] = img_file.read_bytes()

        print("Creating JPEG test EPUB...")
        jpeg_chapters = [
            ("Introduction", make_chapter("JPEG Image Tests", """
<p>This EPUB tests JPEG image rendering.</p>
<p>Navigate through chapters to verify each test case.</p>
<p><strong>Test Plan:</strong></p>
<ul>
<li>Grayscale rendering (4 levels)</li>
<li>Image centering</li>
<li>Large image scaling</li>
<li>Cache performance</li>
</ul>
"""), []),
            ("1. JPEG Format", make_chapter("JPEG Format Test", """
<p>Basic JPEG decoding test.</p>
<img src="images/jpeg_format.jpg" alt="JPEG format test"/>
<p>If the image above is visible, JPEG decoding works.</p>
"""), [('jpeg_format.jpg', images['jpeg_format.jpg'])]),
            ("2. Grayscale", make_chapter("Grayscale Test", """
<p>Verify 4 distinct gray levels are visible.</p>
<img src="images/grayscale_test.jpg" alt="Grayscale test"/>
"""), [('grayscale_test.jpg', images['grayscale_test.jpg'])]),
            ("3. Gradient", make_chapter("Gradient Test", """
<p>Verify gradient quantizes to 4 bands.</p>
<img src="images/gradient_test.jpg" alt="Gradient test"/>
"""), [('gradient_test.jpg', images['gradient_test.jpg'])]),
            ("4. Centering", make_chapter("Centering Test", """
<p>Verify image is centered horizontally.</p>
<img src="images/centering_test.jpg" alt="Centering test"/>
"""), [('centering_test.jpg', images['centering_test.jpg'])]),
            ("5. Scaling", make_chapter("Scaling Test", """
<p>This image is 1200x1500 pixels - larger than the screen.</p>
<p>It should be scaled down to fit.</p>
<img src="images/scaling_test.jpg" alt="Scaling test"/>
"""), [('scaling_test.jpg', images['scaling_test.jpg'])]),
            ("6. Wide Scaling", make_chapter("Wide Scaling Test", """
<p>This image is 1807x736 pixels - a wide landscape format.</p>
<p>Tests scaling with dimensions that can cause cache mismatches.</p>
<img src="images/wide_scaling_test.jpg" alt="Wide scaling test"/>
"""), [('wide_scaling_test.jpg', images['wide_scaling_test.jpg'])]),
            ("7. Cache Test A", make_chapter("Cache Test - Page A", """
<p>First cache test page. Note the load time.</p>
<img src="images/cache_test_1.jpg" alt="Cache test 1"/>
<p>Navigate to next page, then come back.</p>
"""), [('cache_test_1.jpg', images['cache_test_1.jpg'])]),
            ("8. Cache Test B", make_chapter("Cache Test - Page B", """
<p>Second cache test page.</p>
<img src="images/cache_test_2.jpg" alt="Cache test 2"/>
<p>Navigate back to Page A - it should load faster from cache.</p>
"""), [('cache_test_2.jpg', images['cache_test_2.jpg'])]),
            ("9. Alignment Bleed", make_chapter("Image Centering Bleed Test", """
<p>Tests that image centering does not bleed into following text blocks (issue #1026).</p>
<p>Set Paragraph Alignment to Justify and Embedded Style to OFF before testing.</p>
<p>All paragraphs below the images should be justified, not centered.</p>
<h1 class="hidden"></h1>
<p><img src="images/centering_test.jpg" alt="Test image"/></p>
<div>
<p>FIRST PARAGRAPH after image. This paragraph follows an empty heading and an image-only paragraph. With the bug present, this text appears centered instead of justified because the empty heading's default Center alignment bleeds through the chain of empty text blocks. Lorem ipsum dolor sit amet, consectetur adipiscing elit sed do eiusmod tempor.</p>
<p>SECOND PARAGRAPH in the same div. This paragraph should always be justified because the first paragraph's text block was flushed. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident sunt in culpa qui officia.</p>
</div>
"""), []),  # centering_test.jpg already included by chapter 4
        ]

        create_epub(OUTPUT_DIR / 'test_jpeg_images.epub', 'JPEG Image Tests', jpeg_chapters)

        print("Creating PNG test EPUB...")
        png_chapters = [
            ("Introduction", make_chapter("PNG Image Tests", """
<p>This EPUB tests PNG image rendering.</p>
<p>Navigate through chapters to verify each test case.</p>
<p><strong>Test Plan:</strong></p>
<ul>
<li>PNG decoding (no crash)</li>
<li>Grayscale rendering (4 levels)</li>
<li>Image centering</li>
<li>Large image scaling</li>
</ul>
"""), []),
            ("1. PNG Format", make_chapter("PNG Format Test", """
<p>Basic PNG decoding test.</p>
<img src="images/png_format.png" alt="PNG format test"/>
<p>If the image above is visible and no crash occurred, PNG decoding works.</p>
"""), [('png_format.png', images['png_format.png'])]),
            ("2. Grayscale", make_chapter("Grayscale Test", """
<p>Verify 4 distinct gray levels are visible.</p>
<img src="images/grayscale_test.png" alt="Grayscale test"/>
"""), [('grayscale_test.png', images['grayscale_test.png'])]),
            ("3. Gradient", make_chapter("Gradient Test", """
<p>Verify gradient quantizes to 4 bands.</p>
<img src="images/gradient_test.png" alt="Gradient test"/>
"""), [('gradient_test.png', images['gradient_test.png'])]),
            ("4. Centering", make_chapter("Centering Test", """
<p>Verify image is centered horizontally.</p>
<img src="images/centering_test.png" alt="Centering test"/>
"""), [('centering_test.png', images['centering_test.png'])]),
            ("5. Scaling", make_chapter("Scaling Test", """
<p>This image is 1200x1500 pixels - larger than the screen.</p>
<p>It should be scaled down to fit.</p>
<img src="images/scaling_test.png" alt="Scaling test"/>
"""), [('scaling_test.png', images['scaling_test.png'])]),
            ("6. Wide Scaling", make_chapter("Wide Scaling Test", """
<p>This image is 1807x736 pixels - a wide landscape format.</p>
<p>Tests scaling with dimensions that can cause cache mismatches.</p>
<img src="images/wide_scaling_test.png" alt="Wide scaling test"/>
"""), [('wide_scaling_test.png', images['wide_scaling_test.png'])]),
            ("7. Cache Test A", make_chapter("Cache Test - Page A", """
<p>First cache test page. Note the load time.</p>
<img src="images/cache_test_1.png" alt="Cache test 1"/>
<p>Navigate to next page, then come back.</p>
"""), [('cache_test_1.png', images['cache_test_1.png'])]),
            ("8. Cache Test B", make_chapter("Cache Test - Page B", """
<p>Second cache test page.</p>
<img src="images/cache_test_2.png" alt="Cache test 2"/>
<p>Navigate back to Page A - it should load faster from cache.</p>
"""), [('cache_test_2.png', images['cache_test_2.png'])]),
            ("9. Alignment Bleed", make_chapter("Image Centering Bleed Test", """
<p>Tests that image centering does not bleed into following text blocks (issue #1026).</p>
<p>Set Paragraph Alignment to Justify and Embedded Style to OFF before testing.</p>
<p>All paragraphs below the images should be justified, not centered.</p>
<h1 class="hidden"></h1>
<p><img src="images/centering_test.png" alt="Test image"/></p>
<div>
<p>FIRST PARAGRAPH after image. This paragraph follows an empty heading and an image-only paragraph. With the bug present, this text appears centered instead of justified because the empty heading's default Center alignment bleeds through the chain of empty text blocks. Lorem ipsum dolor sit amet, consectetur adipiscing elit sed do eiusmod tempor.</p>
<p>SECOND PARAGRAPH in the same div. This paragraph should always be justified because the first paragraph's text block was flushed. Duis aute irure dolor in reprehenderit in voluptate velit esse cillum dolore eu fugiat nulla pariatur. Excepteur sint occaecat cupidatat non proident sunt in culpa qui officia.</p>
</div>
"""), []),  # centering_test.png already included by chapter 4
        ]

        create_epub(OUTPUT_DIR / 'test_png_images.epub', 'PNG Image Tests', png_chapters)

        print("Creating mixed format test EPUB...")
        mixed_chapters = [
            ("Introduction", make_chapter("Mixed Image Format Tests", """
<p>This EPUB contains both JPEG and PNG images.</p>
<p>Tests format detection and mixed rendering.</p>
"""), []),
            ("1. JPEG Image", make_chapter("JPEG in Mixed EPUB", """
<p>This is a JPEG image:</p>
<img src="images/jpeg_format.jpg" alt="JPEG"/>
"""), [('jpeg_format.jpg', images['jpeg_format.jpg'])]),
            ("2. PNG Image", make_chapter("PNG in Mixed EPUB", """
<p>This is a PNG image:</p>
<img src="images/png_format.png" alt="PNG"/>
"""), [('png_format.png', images['png_format.png'])]),
            ("3. Both Formats", make_chapter("Both Formats on One Page", """
<p>JPEG image:</p>
<img src="images/grayscale_test.jpg" alt="JPEG grayscale"/>
<p>PNG image:</p>
<img src="images/grayscale_test.png" alt="PNG grayscale"/>
<p>Both should render with proper grayscale.</p>
"""), [('grayscale_test.jpg', images['grayscale_test.jpg']),
       ('grayscale_test.png', images['grayscale_test.png'])]),
        ]

        create_epub(OUTPUT_DIR / 'test_mixed_images.epub', 'Mixed Format Tests', mixed_chapters)

        print(f"\nTest EPUBs created in: {OUTPUT_DIR}")
        print("Files:")
        for f in OUTPUT_DIR.glob('*.epub'):
            print(f"  - {f.name}")

if __name__ == '__main__':
    main()
