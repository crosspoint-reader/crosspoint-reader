#!/usr/bin/env python3
"""
Generate CJK UI font header file for CrossPoint Reader.
Uses Source Han Sans (思源黑体) to generate bitmap font data.

Usage:
    python3 generate_cjk_ui_font.py --size 26 --font /path/to/SourceHanSansSC-Medium.otf
"""

import argparse
import sys
from pathlib import Path

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    print("Error: PIL/Pillow not installed. Run: pip3 install Pillow")
    sys.exit(1)

# UI characters needed (extracted from I18n strings + common punctuation)
UI_CHARS = """
!%&()*+,-./0123456789:;<=>?@，。！、：；？""''「」『』【】〈〉《》〔〕…—―─·•
ABCDEFGHIJKLMNOPQRSTUVWXYZ[]{}_
abcdefghijklmnopqrstuvwxyz|«»
あいうえおかきくけこさしすせそたちつてとなにぬねのはひふへほまみむめもやゆよらりるれろわをん
がぎぐげござじずぜぞだぢづでどばびぶべぼぱぴぷぺぽ
ぁぃぅぇぉっゃゅょゔ
アイウエオカキクケコサシスセソタチツテトナニヌネノハヒフヘホマミムメモヤユヨラリルレロワヲン
ガギグゲゴザジズゼゾダヂヅデドバビブベボパピプペポ
ァィゥェォッャュョヴヵヶー
一二三四五六七八九十百千万亿
简体中文日本語启动休眠进入浏览件传输设置书库继续阅读无打开的籍从下方始未找到选择章节已末空索引内存错误页面加载超出范围失败卡网络个扫描连接时忘记保密码删除按确定重新任意键左右认式创建热点现有供他人模将备此在器或用手机维线地址作为检查字正搜等待指令试断收更多容需要显示控制系统屏幕封状态栏隐藏电量百分比段落额外间距抗锯齿源短向前钮布局侧边长跳转大小行母数汉颜色对齐符刷频率同步语言壁纸清理缓户名服务档匹配证请先凭据成功就绪完这所度丢当再次项看串口了解详情深浅自义适应裁剪整不终忽略翻竖横顺针倒逆返回上特紧凑常宽松两端居钟版可是最禁条目命获取订析退主切换消否关写起動覧転送設続読開書見選択終込範囲敗削押確認法参既暗号化済力検機試受信必画隠追間隔電側長漢余白時頻期紙去証初報利能進捗項詳細無視縦計反戻狭普通広両揃え央現部蔵効題得替決
远程本应置账户配来自
リモートローカルセクションアップロード元
仅付位修光典刻含哈埋复射嵌希提映样滤界移算经缩褪计过近適镜阳题首（）
"""

# Extract unique characters
def get_unique_chars(text):
    chars = set()
    for c in text:
        if c.strip() and ord(c) >= 0x20:
            chars.add(c)
    return sorted(chars, key=ord)

def load_font_fitting_cell(font_path, pixel_size):
    """Load a font and shrink it until ascent+descent fits the cell height."""
    pt_size = max(1, int(pixel_size))
    while pt_size > 0:
        try:
            font = ImageFont.truetype(font_path, pt_size)
        except Exception as e:
            print(f"Error loading font: {e}")
            return None, None, None, None
        ascent, descent = font.getmetrics()
        if ascent + descent <= pixel_size:
            return font, pt_size, ascent, descent
        pt_size -= 1
    return None, None, None, None

def generate_font_header(font_path, pixel_size, output_path):
    """Generate CJK UI font header file."""

    font, pt_size, ascent, descent = load_font_fitting_cell(font_path, pixel_size)
    if font is None:
        return False

    chars = get_unique_chars(UI_CHARS)
    print(f"Generating {pixel_size}x{pixel_size} font with {len(chars)} characters...")

    # Collect glyph data
    codepoints = []
    widths = []
    bitmaps = []

    # Get font metrics for consistent vertical alignment
    font_height = ascent + descent
    # Fixed baseline (from top): align all glyphs to the same baseline to avoid jitter
    baseline = pixel_size - descent

    for char in chars:
        cp = ord(char)

        # Create image for character
        img = Image.new('1', (pixel_size, pixel_size), 0)
        draw = ImageDraw.Draw(img)

        # Get character bounding box
        try:
            bbox = font.getbbox(char)
            if bbox:
                char_width = bbox[2] - bbox[0]
                char_height = bbox[3] - bbox[1]
            else:
                char_width = pixel_size // 2
                char_height = pixel_size
        except:
            char_width = pixel_size // 2
            char_height = pixel_size

        # Left align with 1px padding if space allows
        x = 0 if char_width > pixel_size - 2 else 1
        # Align to a fixed baseline to keep CJK/Latin stable within the same line
        y = baseline

        # Draw character
        try:
            # Use left-baseline anchor: y is the baseline position
            draw.text((x, y), char, font=font, fill=1, anchor="ls")
        except TypeError:
            # Fallback for older Pillow: approximate baseline by shifting up
            draw.text((x, y - ascent), char, font=font, fill=1)

        # Convert to bytes
        bytes_per_row = (pixel_size + 7) // 8
        bitmap_bytes = []
        for row in range(pixel_size):
            for byte_idx in range(bytes_per_row):
                byte_val = 0
                for bit in range(8):
                    px = byte_idx * 8 + bit
                    if px < pixel_size:
                        pixel = img.getpixel((px, row))
                        if pixel:
                            byte_val |= (1 << (7 - bit))
                bitmap_bytes.append(byte_val)

        codepoints.append(cp)
        # Calculate advance width
        if cp < 0x80:
            # ASCII: use actual width + small padding
            widths.append(min(char_width + 2, pixel_size))
        else:
            # CJK: use full width
            widths.append(pixel_size)
        bitmaps.append(bitmap_bytes)

    # Generate header file
    bytes_per_row = (pixel_size + 7) // 8
    bytes_per_char = bytes_per_row * pixel_size

    with open(output_path, 'w') as f:
        f.write(f'''/**
 * Auto-generated CJK UI font data (optimized - UI characters only)
 * Font: 思源黑体-Medium
 * Size: {pt_size}pt
 * Dimensions: {pixel_size}x{pixel_size}
 * Characters: {len(chars)}
 * Total size: {len(chars) * bytes_per_char} bytes ({len(chars) * bytes_per_char / 1024:.1f} KB)
 *
 * This is a sparse font containing only UI-required CJK characters.
 * Uses a lookup table for codepoint -> glyph index mapping.
 * Supports proportional spacing for English characters.
 */
#pragma once
namespace CjkUiFont{pixel_size} {{

#include <cstdint>
#include <pgmspace.h>

// Font parameters
static constexpr uint8_t CJK_UI_FONT_WIDTH = {pixel_size};
static constexpr uint8_t CJK_UI_FONT_HEIGHT = {pixel_size};
static constexpr uint8_t CJK_UI_FONT_BYTES_PER_ROW = {bytes_per_row};
static constexpr uint8_t CJK_UI_FONT_BYTES_PER_CHAR = {bytes_per_char};
static constexpr uint16_t CJK_UI_FONT_GLYPH_COUNT = {len(chars)};

// Codepoint lookup table (sorted for binary search)
static const uint16_t CJK_UI_CODEPOINTS[] PROGMEM = {{
''')

        # Write codepoints
        for i, cp in enumerate(codepoints):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'0x{cp:04X}, ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        if len(codepoints) % 16 != 0:
            f.write('\n')
        f.write('};\n\n')

        # Write widths
        f.write('// Glyph width table (actual advance width for proportional spacing)\n')
        f.write('static const uint8_t CJK_UI_GLYPH_WIDTHS[] PROGMEM = {\n')
        for i, w in enumerate(widths):
            if i % 16 == 0:
                f.write('    ')
            f.write(f'{w:3}, ')
            if (i + 1) % 16 == 0:
                f.write('\n')
        if len(widths) % 16 != 0:
            f.write('\n')
        f.write('};\n\n')

        # Write bitmap data
        f.write('// Glyph bitmap data\n')
        f.write('static const uint8_t CJK_UI_GLYPHS[] PROGMEM = {\n')
        for i, bitmap in enumerate(bitmaps):
            f.write(f'    // U+{codepoints[i]:04X} ({chr(codepoints[i])})\n    ')
            for j, b in enumerate(bitmap):
                f.write(f'0x{b:02X}, ')
                if (j + 1) % 16 == 0 and j < len(bitmap) - 1:
                    f.write('\n    ')
            f.write('\n')
        f.write('};\n\n')

        # Write lookup functions
        f.write('''// Binary search for codepoint
inline int findGlyphIndex(uint16_t codepoint) {
    int low = 0;
    int high = CJK_UI_FONT_GLYPH_COUNT - 1;
    while (low <= high) {
        int mid = (low + high) / 2;
        uint16_t midCp = pgm_read_word(&CJK_UI_CODEPOINTS[mid]);
        if (midCp == codepoint) return mid;
        if (midCp < codepoint) low = mid + 1;
        else high = mid - 1;
    }
    return -1;
}

inline bool hasCjkUiGlyph(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return false;
    return findGlyphIndex(static_cast<uint16_t>(codepoint)) >= 0;
}

inline const uint8_t* getCjkUiGlyph(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return nullptr;
    int idx = findGlyphIndex(static_cast<uint16_t>(codepoint));
    if (idx < 0) return nullptr;
    return &CJK_UI_GLYPHS[idx * CJK_UI_FONT_BYTES_PER_CHAR];
}

inline uint8_t getCjkUiGlyphWidth(uint32_t codepoint) {
    if (codepoint > 0xFFFF) return 0;
    int idx = findGlyphIndex(static_cast<uint16_t>(codepoint));
    if (idx < 0) return 0;
    return pgm_read_byte(&CJK_UI_GLYPH_WIDTHS[idx]);
}

} // namespace CjkUiFont''' + str(pixel_size) + '\n')

    print(f"Generated: {output_path}")
    print(f"  - {len(chars)} characters")
    print(f"  - {len(chars) * bytes_per_char} bytes bitmap data")
    return True

def main():
    parser = argparse.ArgumentParser(description='Generate CJK UI font header')
    parser.add_argument('--size', type=int, default=26, help='Pixel size (default: 26)')
    parser.add_argument('--font', type=str, required=True, help='Path to Source Han Sans font file')
    parser.add_argument('--output', type=str, help='Output path (default: lib/GfxRenderer/cjk_ui_font_SIZE.h)')
    args = parser.parse_args()

    script_dir = Path(__file__).parent
    project_root = script_dir.parent

    if args.output:
        output_path = Path(args.output)
    else:
        output_path = project_root / 'lib' / 'GfxRenderer' / f'cjk_ui_font_{args.size}.h'

    if not Path(args.font).exists():
        print(f"Error: Font file not found: {args.font}")
        sys.exit(1)

    if generate_font_header(args.font, args.size, output_path):
        print("Success!")
    else:
        print("Failed!")
        sys.exit(1)

if __name__ == '__main__':
    main()
