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
!#$%&'()*+,-./0123456789:;<=>?@
ABCDEFGHIJKLMNOPQRSTUVWXYZ[\]^_`
abcdefghijklmnopqrstuvwxyz{|}~
启动中休眠进入浏览文件传输设置书库继续阅读无打开的籍从下方开始
未找到选择章节空已末索引内存错误页面加载
网络扫描连接失败超时忘记保存密码删除按确定重新任意键
左右上确认加热点现有供他人
此址或手机二维码地检查输入文字
正在搜寻等待指令断更多容创建需要中
屏幕封显示模式状态栏隐藏电量百分比段落额外间距抗锯齿
电源短阅读方向前置按钮布局侧边长跳转字体大小行
颜色边距对齐时间刷新频率语言壁纸清理缓存
深浅自定义适应裁剪度完整从不始终忽略翻竖横顺逆针
返确左右上一下紧凑正常宽松两端居分钟
版本新可用当前中检查数据成功信息
外内禁停全最后退出主保切换取消打回重试是否关
大写小决
英简繁體日本語
个令信先写制力务卡去受同名命壁多容待得必指控搜收数断服期析步母汉清理符等简系纸统缓获要解订需颜
ファイル一覧転送定ライブラリ続読開本ありません下書始
見つかりませんを章なし終わり空のインデックスメモリエラー
ページ込み範囲外失敗しました
ネットワークスキャン接続完了タイムアウト忘れるパスワード
押して再任意ボタン行方法参加ホットスポット作成既存
デバイスブラウザこのURLまたはスマホQRコードをして
無線ケーブル送受信待機
画面カバー非表示常にベル配置向きボン前後サイド押し飛ばし
フォントサイズ幅余白揃え分
アップート利チェック新しいバージョン現更失敗完成功
フ使用可中壊オフオンセ解除左右上回戻キャンセル再度はいいいえ
検機決漢紙題
"""

# Extract unique characters
def get_unique_chars(text):
    chars = set()
    for c in text:
        if c.strip() and ord(c) >= 0x20:
            chars.add(c)
    return sorted(chars, key=ord)

def generate_font_header(font_path, pixel_size, output_path):
    """Generate CJK UI font header file."""

    # Calculate font point size (approximately pixel_size * 0.7)
    pt_size = int(pixel_size * 0.7)

    try:
        font = ImageFont.truetype(font_path, pt_size)
    except Exception as e:
        print(f"Error loading font: {e}")
        return False

    chars = get_unique_chars(UI_CHARS)
    print(f"Generating {pixel_size}x{pixel_size} font with {len(chars)} characters...")

    # Collect glyph data
    codepoints = []
    widths = []
    bitmaps = []

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

        # Center character in cell
        x = (pixel_size - char_width) // 2
        y = (pixel_size - char_height) // 2 - (bbox[1] if bbox else 0)

        # Draw character
        draw.text((x, y), char, font=font, fill=1)

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
