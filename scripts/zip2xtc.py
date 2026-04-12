#!/usr/bin/env python3
"""
zip2xtc.py - ZIP/CBZ画像アーカイブ → XTC/XTCH 変換・検証ツール

CrossPoint Reader向けのXTC/XTCHファイルを生成する。
画像を含むZIP（またはCBZ）ファイルを入力として、
E-Ink表示に最適化されたXTCコンテナファイルを出力する。

フォーマット仕様: lib/Xtc/Xtc/XtcTypes.h に準拠

使用例:
  # 1ビットXTC変換（漫画、右→左）
  python3 scripts/zip2xtc.py input.zip -o output.xtc --rtl --title "書名" --author "著者"

  # 2ビットXTCH変換（グレースケール）
  python3 scripts/zip2xtc.py input.zip -o output.xtch --xtch --rtl

  # 既存XTCファイルの検証
  python3 scripts/zip2xtc.py --verify output.xtc

  # 検証 + ページ抽出
  python3 scripts/zip2xtc.py --verify output.xtc --extract-page 0 -o page0.png
"""

import argparse
import os
import re
import struct
import sys
import time
import zipfile
from io import BytesIO

from PIL import Image, ImageEnhance, ImageOps

# ============================================================
# XTC フォーマット定数 (lib/Xtc/Xtc/XtcTypes.h 準拠)
# ============================================================

# マジックナンバー (リトルエンディアン)
XTC_MAGIC  = 0x00435458  # "XTC\0"
XTCH_MAGIC = 0x48435458  # "XTCH"
XTG_MAGIC  = 0x00475458  # "XTG\0" (1-bit page)
XTH_MAGIC  = 0x00485458  # "XTH\0" (2-bit page)

# デフォルト表示解像度 (XTeink X4)
DISPLAY_WIDTH  = 480
DISPLAY_HEIGHT = 800

# 構造体サイズ
HEADER_SIZE          = 56   # XtcHeader
METADATA_SIZE        = 256  # メタデータセクション
PAGE_TABLE_ENTRY_SIZE = 16  # PageTableEntry
PAGE_HEADER_SIZE     = 22   # XtgPageHeader

# 読み方向
READ_LTR = 0  # 左→右
READ_RTL = 1  # 右→左 (漫画)
READ_TTB = 2  # 上→下

# 画像拡張子
IMAGE_EXTENSIONS = {'.jpg', '.jpeg', '.png', '.gif', '.bmp', '.webp', '.tiff'}


# ============================================================
# ユーティリティ
# ============================================================

def natural_sort_key(s: str):
    """自然順ソートのキー関数（数値部分を数値として比較）"""
    return [
        int(part) if part.isdigit() else part.lower()
        for part in re.split(r'(\d+)', s)
    ]


def format_size(size_bytes: int) -> str:
    """バイト数を人間が読みやすい形式に変換"""
    if size_bytes < 1024:
        return f"{size_bytes} B"
    elif size_bytes < 1024 * 1024:
        return f"{size_bytes / 1024:.1f} KB"
    else:
        return f"{size_bytes / (1024 * 1024):.1f} MB"


# ============================================================
# ZIP操作
# ============================================================

def list_images_in_zip(zip_path: str) -> list[str]:
    """ZIP内の画像ファイルを自然順でリストする"""
    with zipfile.ZipFile(zip_path, 'r') as zf:
        images = []
        for name in zf.namelist():
            if name.endswith('/'):
                continue
            ext = os.path.splitext(name)[1].lower()
            if ext in IMAGE_EXTENSIONS:
                images.append(name)

    # ファイル名部分で自然順ソート
    images.sort(key=lambda p: natural_sort_key(os.path.basename(p)))
    return images


def load_image_from_zip(zf: zipfile.ZipFile, name: str) -> Image.Image:
    """ZIPから画像を読み込む"""
    data = zf.read(name)
    return Image.open(BytesIO(data))


# ============================================================
# 画像処理
# ============================================================

def trim_whitespace(img: Image.Image, threshold: int = 250, padding: int = 2) -> Image.Image:
    """画像の白い余白を自動トリミングする

    Args:
        img: グレースケール画像
        threshold: この値以上のピクセルを背景とみなす (0-255)
        padding: トリミング後に残す余白ピクセル数
    """
    # threshold未満のピクセル（=コンテンツ）のbboxを取得
    # point()で二値化: コンテンツ=255, 背景=0
    mask = img.point(lambda p: 255 if p < threshold else 0)
    bbox = mask.getbbox()
    if bbox is None:
        return img  # 全面白 → トリミングなし

    # パディング付きでクロップ
    x0 = max(0, bbox[0] - padding)
    y0 = max(0, bbox[1] - padding)
    x1 = min(img.width, bbox[2] + padding)
    y1 = min(img.height, bbox[3] + padding)

    return img.crop((x0, y0, x1, y1))


def process_image(img: Image.Image, width: int, height: int,
                  auto_trim: bool = True, fill: bool = False) -> Image.Image:
    """画像をターゲットサイズにリサイズし、グレースケール変換する

    fill=False (fit): アスペクト比維持、余白は白で埋め（レターボックス）
    fill=True:        アスペクト比維持、ターゲットを完全に埋める（端をクロップ）
    auto_trim=True の場合、リサイズ前に白い余白を自動除去する。
    """
    # グレースケール変換
    img = img.convert('L')

    # 白余白の自動トリミング
    if auto_trim:
        img = trim_whitespace(img)

    # コントラスト補正: autocontrast + 1.3x強化
    img = ImageOps.autocontrast(img, cutoff=1)
    img = ImageEnhance.Contrast(img).enhance(1.3)

    img_ratio = img.width / img.height
    target_ratio = width / height

    if fill:
        # フィルモード: ターゲットを完全に埋め、はみ出た部分をクロップ
        if img_ratio > target_ratio:
            # 横長 → 高さに合わせ、左右をクロップ
            new_height = height
            new_width = int(height * img_ratio)
        else:
            # 縦長 → 幅に合わせ、上下をクロップ
            new_width = width
            new_height = int(width / img_ratio)

        img = img.resize((new_width, new_height), Image.LANCZOS)

        # 中央クロップ
        crop_x = (new_width - width) // 2
        crop_y = (new_height - height) // 2
        result = img.crop((crop_x, crop_y, crop_x + width, crop_y + height))
    else:
        # フィットモード: 余白ありレターボックス
        if img_ratio > target_ratio:
            new_width = width
            new_height = int(width / img_ratio)
        else:
            new_height = height
            new_width = int(height * img_ratio)

        img = img.resize((new_width, new_height), Image.LANCZOS)

        result = Image.new('L', (width, height), 255)
        offset_x = (width - new_width) // 2
        offset_y = (height - new_height) // 2
        result.paste(img, (offset_x, offset_y))

    return result


# ============================================================
# XTG エンコード (1-bit)
# ============================================================

def encode_xtg(img_gray: Image.Image) -> bytes:
    """グレースケール画像を1ビットXTGデータにエンコード

    Floyd-Steinbergディザリング → 1ビットパック
    行優先、8ピクセル/バイト、MSBファースト
    0=黒、1=白

    PILのmode '1'変換とraw '1'パッカーを使用して高速処理。
    """
    # Floyd-Steinbergディザリングで1ビット化
    img_bw = img_gray.convert('1')
    width, height = img_bw.size

    # PILの raw '1' パッカー: 8px/byte, MSB first, 0=black, 1=white
    # XTGフォーマットと完全一致
    data = img_bw.tobytes('raw', '1')

    expected_size = ((width + 7) // 8) * height
    if len(data) != expected_size:
        # フォールバック: 手動パック
        data = _encode_xtg_manual(img_bw)

    return data


def _encode_xtg_manual(img_bw: Image.Image) -> bytes:
    """XTGデータの手動パック（PILのrawパッカーが期待通りでない場合のフォールバック）"""
    width, height = img_bw.size
    bytes_per_row = (width + 7) // 8
    result = bytearray(bytes_per_row * height)

    # mode 'L' に変換して確実なピクセルアクセス
    img_l = img_bw.convert('L')
    raw = img_l.tobytes()

    for y in range(height):
        for x in range(width):
            pixel = raw[y * width + x]
            if pixel > 0:  # White
                byte_idx = y * bytes_per_row + x // 8
                bit_idx = 7 - (x % 8)  # MSB first
                result[byte_idx] |= (1 << bit_idx)

    return bytes(result)


# ============================================================
# XTH エンコード (2-bit)
# ============================================================

def encode_xth(img_gray: Image.Image) -> bytes:
    """グレースケール画像を2ビットXTHデータにエンコード

    4レベル量子化 → 2ビットプレーン分離
    列優先（右→左）、8垂直ピクセル/バイト、MSBファースト

    グレースケール値: 0=White, 1=DarkGrey, 2=LightGrey, 3=Black
    pixelValue = (bit1 << 1) | bit2
    """
    width, height = img_gray.size

    # 8bit → 2bit 量子化テーブル
    # 0=White, 1=DarkGrey, 2=LightGrey, 3=Black
    quantize_table = bytearray(256)
    for v in range(256):
        if v >= 192:
            quantize_table[v] = 0   # White
        elif v >= 128:
            quantize_table[v] = 2   # Light Grey
        elif v >= 64:
            quantize_table[v] = 1   # Dark Grey
        else:
            quantize_table[v] = 3   # Black

    raw = img_gray.tobytes()

    # 2ビット量子化
    quantized = bytearray(width * height)
    for i in range(len(raw)):
        quantized[i] = quantize_table[raw[i]]

    # ビットプレーンサイズ
    total_pixels = width * height
    plane_size = (total_pixels + 7) // 8
    plane1 = bytearray(plane_size)  # bit1 (MSB)
    plane2 = bytearray(plane_size)  # bit2 (LSB)

    # 列優先（右→左）、8垂直ピクセル/バイト、MSBファースト
    bit_index = 0
    for x in range(width - 1, -1, -1):  # Right to left
        for y in range(height):
            val = quantized[y * width + x]
            byte_idx = bit_index >> 3
            bit_pos = 7 - (bit_index & 7)  # MSB first

            if val & 2:  # bit1
                plane1[byte_idx] |= (1 << bit_pos)
            if val & 1:  # bit2
                plane2[byte_idx] |= (1 << bit_pos)

            bit_index += 1

    return bytes(plane1) + bytes(plane2)


# ============================================================
# XTCコンテナ構築
# ============================================================

def build_page_data(page_magic: int, width: int, height: int, bitmap: bytes) -> bytes:
    """XTG/XTHページデータ（ヘッダー + ビットマップ）を構築

    XtgPageHeader (22 bytes):
      uint32_t magic
      uint16_t width
      uint16_t height
      uint8_t  colorMode  (0)
      uint8_t  compression (0)
      uint32_t dataSize
      uint64_t md5  (0, optional)
    """
    header = struct.pack('<IHHBBI8s',
        page_magic,
        width,
        height,
        0,             # colorMode
        0,             # compression
        len(bitmap),   # dataSize
        b'\x00' * 8   # md5 (unused)
    )
    assert len(header) == PAGE_HEADER_SIZE
    return header + bitmap


def build_xtc_file(pages_bitmap: list[tuple[int, int, bytes]],
                   is_xtch: bool,
                   read_direction: int,
                   title: str,
                   author: str) -> bytes:
    """XTC/XTCHコンテナファイルを構築

    バイナリレイアウト:
      [0x00]           XtcHeader (56B)
      [0x38]           Metadata (256B): title@0(128B), author@128(64B), ...
      [0x138]          PageTable (16B × N)
      [0x138 + 16*N]   Page0: XtgPageHeader(22B) + bitmap
      [...]            Page1, Page2, ...

    Args:
        pages_bitmap: [(width, height, bitmap_bytes), ...] 各ページ
        is_xtch: True=XTCH(2-bit), False=XTC(1-bit)
        read_direction: 0=LTR, 1=RTL, 2=TTB
        title: タイトル (UTF-8, max 127 bytes)
        author: 著者名 (UTF-8, max 63 bytes)
    """
    page_count = len(pages_bitmap)
    file_magic = XTCH_MAGIC if is_xtch else XTC_MAGIC
    page_magic = XTH_MAGIC if is_xtch else XTG_MAGIC

    # オフセット計算
    metadata_offset = HEADER_SIZE                                       # 0x38
    page_table_offset = metadata_offset + METADATA_SIZE                 # 0x138
    data_offset = page_table_offset + PAGE_TABLE_ENTRY_SIZE * page_count

    # メタデータセクション (256 bytes)
    metadata = bytearray(METADATA_SIZE)
    title_bytes = title.encode('utf-8')[:127]
    author_bytes = author.encode('utf-8')[:63]
    metadata[0:len(title_bytes)] = title_bytes
    metadata[0x80:0x80 + len(author_bytes)] = author_bytes

    # ページデータ構築
    pages_data = []
    for width, height, bitmap in pages_bitmap:
        page_bytes = build_page_data(page_magic, width, height, bitmap)
        pages_data.append(page_bytes)

    # ページテーブル構築
    # PageTableEntry (16 bytes):
    #   uint64_t dataOffset
    #   uint32_t dataSize
    #   uint16_t width
    #   uint16_t height
    page_table = bytearray()
    current_offset = data_offset
    for i, page_bytes in enumerate(pages_data):
        w, h, _ = pages_bitmap[i]
        entry = struct.pack('<QIHH',
            current_offset,
            len(page_bytes),
            w,
            h,
        )
        assert len(entry) == PAGE_TABLE_ENTRY_SIZE
        page_table += entry
        current_offset += len(page_bytes)

    # XtcHeader (56 bytes)
    # struct layout:
    #   uint32_t magic           (4)
    #   uint8_t  versionMajor    (1)
    #   uint8_t  versionMinor    (1)
    #   uint16_t pageCount       (2)
    #   uint8_t  readDirection   (1)
    #   uint8_t  hasMetadata     (1)
    #   uint8_t  hasThumbnails   (1)
    #   uint8_t  hasChapters     (1)
    #   uint32_t currentPage     (4)
    #   uint64_t metadataOffset  (8)
    #   uint64_t pageTableOffset (8)
    #   uint64_t dataOffset      (8)
    #   uint64_t thumbOffset     (8)
    #   uint32_t chapterOffset   (4)
    #   uint32_t padding         (4)
    header = struct.pack('<IBBHBBBBIQQQQII',
        file_magic,
        1,                   # versionMajor
        0,                   # versionMinor
        page_count,
        read_direction,
        1,                   # hasMetadata
        0,                   # hasThumbnails
        0,                   # hasChapters
        1,                   # currentPage (1-based)
        metadata_offset,
        page_table_offset,
        data_offset,
        0,                   # thumbOffset (unused)
        0,                   # chapterOffset (unused)
        0,                   # padding
    )
    assert len(header) == HEADER_SIZE

    # ファイル組み立て
    output = bytearray()
    output += header
    output += metadata
    output += page_table
    for page_bytes in pages_data:
        output += page_bytes

    return bytes(output)


# ============================================================
# 変換メイン処理
# ============================================================

def convert_zip_to_xtc(zip_path: str,
                       output_path: str,
                       is_xtch: bool = False,
                       read_direction: int = READ_LTR,
                       title: str = "",
                       author: str = "",
                       target_width: int = DISPLAY_WIDTH,
                       target_height: int = DISPLAY_HEIGHT,
                       auto_trim: bool = True) -> None:
    """ZIP画像アーカイブをXTC/XTCHファイルに変換"""

    images = list_images_in_zip(zip_path)
    if not images:
        print("エラー: ZIP内に画像ファイルが見つかりません", file=sys.stderr)
        sys.exit(1)

    print(f"入力: {zip_path}")
    print(f"画像数: {len(images)}")
    print(f"出力: {output_path}")
    print(f"モード: {'XTCH (2-bit)' if is_xtch else 'XTC (1-bit)'}")
    print(f"解像度: {target_width}x{target_height}")
    print(f"読み方向: {'RTL (右→左)' if read_direction == READ_RTL else 'LTR (左→右)'}")
    print(f"自動トリミング: {'ON' if auto_trim else 'OFF'}")
    print()

    pages_bitmap = []
    start_time = time.time()

    with zipfile.ZipFile(zip_path, 'r') as zf:
        for i, name in enumerate(images):
            basename = os.path.basename(name)
            elapsed = time.time() - start_time
            if i > 0:
                eta = elapsed / i * (len(images) - i)
                print(f"\r  [{i+1}/{len(images)}] {basename:<40s} (残り {eta:.0f}秒)", end='', flush=True)
            else:
                print(f"\r  [{i+1}/{len(images)}] {basename:<40s}", end='', flush=True)

            img = load_image_from_zip(zf, name)
            img_gray = process_image(img, target_width, target_height, auto_trim=auto_trim)
            img.close()

            if is_xtch:
                bitmap = encode_xth(img_gray)
            else:
                bitmap = encode_xtg(img_gray)

            pages_bitmap.append((target_width, target_height, bitmap))
            img_gray.close()

    total_time = time.time() - start_time
    print(f"\r  変換完了: {len(images)}ページ ({total_time:.1f}秒)                    ")

    # XTCファイル構築
    print("XTCコンテナを構築中...")
    xtc_data = build_xtc_file(
        pages_bitmap,
        is_xtch=is_xtch,
        read_direction=read_direction,
        title=title,
        author=author,
    )

    with open(output_path, 'wb') as f:
        f.write(xtc_data)

    print(f"出力: {output_path} ({format_size(len(xtc_data))})")


# ============================================================
# 検証
# ============================================================

def verify_xtc(file_path: str, extract_page: int = -1, extract_output: str = "") -> bool:
    """XTC/XTCHファイルの整合性を検証する

    XtcParser.cpp の読み込みロジックと同じ手順で検証。
    """
    print(f"検証: {file_path}")
    print(f"サイズ: {format_size(os.path.getsize(file_path))}")
    print()

    ok = True

    with open(file_path, 'rb') as f:
        file_size = os.path.getsize(file_path)

        # --- ヘッダー検証 ---
        header_raw = f.read(HEADER_SIZE)
        if len(header_raw) != HEADER_SIZE:
            print(f"  ✗ ヘッダーが短すぎる: {len(header_raw)} bytes (期待: {HEADER_SIZE})")
            return False

        (magic, ver_major, ver_minor, page_count,
         read_dir, has_meta, has_thumb, has_chap,
         current_page, meta_off, pt_off, data_off,
         thumb_off, chap_off, padding) = struct.unpack('<IBBHBBBBIQQQQII', header_raw)

        # マジック
        if magic == XTC_MAGIC:
            fmt_name = "XTC (1-bit)"
            bit_depth = 1
            expected_page_magic = XTG_MAGIC
        elif magic == XTCH_MAGIC:
            fmt_name = "XTCH (2-bit)"
            bit_depth = 2
            expected_page_magic = XTH_MAGIC
        else:
            print(f"  ✗ 不正なマジック: 0x{magic:08X} (期待: 0x{XTC_MAGIC:08X} or 0x{XTCH_MAGIC:08X})")
            return False
        print(f"  ✓ マジック: {fmt_name}")

        # バージョン
        valid_ver = (ver_major == 1 and ver_minor == 0) or (ver_major == 0 and ver_minor == 1)
        if valid_ver:
            print(f"  ✓ バージョン: {ver_major}.{ver_minor}")
        else:
            print(f"  ✗ 不正なバージョン: {ver_major}.{ver_minor}")
            ok = False

        # ページ数
        if page_count == 0:
            print(f"  ✗ ページ数が0")
            ok = False
        else:
            print(f"  ✓ ページ数: {page_count}")

        # 読み方向
        dir_names = {0: "LTR", 1: "RTL", 2: "TTB"}
        print(f"  ✓ 読み方向: {dir_names.get(read_dir, f'unknown({read_dir})')}")

        # フラグ
        print(f"    メタデータ: {'あり' if has_meta else 'なし'}")
        print(f"    サムネイル: {'あり' if has_thumb else 'なし'}")
        print(f"    チャプター: {'あり' if has_chap else 'なし'}")
        print(f"    現在ページ: {current_page}")

        # オフセット検証
        print(f"    metadataOffset:  0x{meta_off:X}")
        print(f"    pageTableOffset: 0x{pt_off:X}")
        print(f"    dataOffset:      0x{data_off:X}")
        print()

        if meta_off >= file_size:
            print(f"  ✗ metadataOffsetがファイルサイズを超過")
            ok = False
        if pt_off >= file_size:
            print(f"  ✗ pageTableOffsetがファイルサイズを超過")
            ok = False
        if data_off > file_size:
            print(f"  ✗ dataOffsetがファイルサイズを超過")
            ok = False

        # --- メタデータ検証 ---
        if has_meta and meta_off > 0:
            f.seek(meta_off)
            title_buf = f.read(128)
            title = title_buf.split(b'\x00')[0].decode('utf-8', errors='replace')
            f.seek(meta_off + 0x80)
            author_buf = f.read(64)
            author = author_buf.split(b'\x00')[0].decode('utf-8', errors='replace')
            print(f"  メタデータ:")
            print(f"    タイトル: {title}")
            print(f"    著者:     {author}")
            print()

        # --- ページテーブル検証 ---
        f.seek(pt_off)
        page_table = []
        for i in range(page_count):
            entry_raw = f.read(PAGE_TABLE_ENTRY_SIZE)
            if len(entry_raw) != PAGE_TABLE_ENTRY_SIZE:
                print(f"  ✗ ページテーブルエントリ {i} の読み込み失敗")
                ok = False
                break
            d_offset, d_size, p_width, p_height = struct.unpack('<QIHH', entry_raw)
            page_table.append((d_offset, d_size, p_width, p_height))

        if page_table:
            # 最初と最後のページ情報を表示
            d0, s0, w0, h0 = page_table[0]
            print(f"  ページテーブル ({len(page_table)} entries):")
            print(f"    Page 0:   offset=0x{d0:X}, size={s0}, {w0}x{h0}")
            if len(page_table) > 1:
                dl, sl, wl, hl = page_table[-1]
                print(f"    Page {len(page_table)-1}: offset=0x{dl:X}, size={sl}, {wl}x{hl}")

            # オフセット整合性チェック
            for i, (d_off, d_size, pw, ph) in enumerate(page_table):
                if d_off + d_size > file_size:
                    print(f"  ✗ Page {i}: データがファイル末尾を超過 (0x{d_off:X} + {d_size} > {file_size})")
                    ok = False
                    break
            else:
                print(f"  ✓ ページテーブルのオフセット整合性OK")
            print()

        # --- ページデータ検証 ---
        errors = 0
        for i, (d_off, d_size, pw, ph) in enumerate(page_table):
            f.seek(d_off)
            ph_raw = f.read(PAGE_HEADER_SIZE)
            if len(ph_raw) != PAGE_HEADER_SIZE:
                print(f"  ✗ Page {i}: ページヘッダー読み込み失敗")
                errors += 1
                continue

            p_magic, p_w, p_h, p_cm, p_comp, p_dsize = struct.unpack('<IHHBBI', ph_raw[:14])
            # md5は残り8バイト (検証不要)

            if p_magic != expected_page_magic:
                print(f"  ✗ Page {i}: 不正なマジック 0x{p_magic:08X} (期待: 0x{expected_page_magic:08X})")
                errors += 1
                continue

            # ビットマップサイズ検証
            if bit_depth == 1:
                expected_bitmap_size = ((p_w + 7) // 8) * p_h
            else:
                expected_bitmap_size = ((p_w * p_h + 7) // 8) * 2

            if p_dsize != expected_bitmap_size:
                print(f"  ✗ Page {i}: dataSize不一致 {p_dsize} (期待: {expected_bitmap_size})")
                errors += 1
                continue

            # ページデータサイズ = ヘッダー + ビットマップ
            expected_page_size = PAGE_HEADER_SIZE + expected_bitmap_size
            if d_size != expected_page_size:
                print(f"  ✗ Page {i}: ページサイズ不一致 {d_size} (期待: {expected_page_size})")
                errors += 1

        if errors == 0:
            print(f"  ✓ 全{page_count}ページのデータ整合性OK")
        else:
            print(f"  ✗ {errors}ページでエラー検出")
            ok = False

        # --- ページ抽出 ---
        if extract_page >= 0 and extract_page < page_count:
            print()
            print(f"  ページ {extract_page} を抽出中...")
            d_off, d_size, pw, ph = page_table[extract_page]
            f.seek(d_off + PAGE_HEADER_SIZE)

            if bit_depth == 1:
                bitmap_size = ((pw + 7) // 8) * ph
                bitmap = f.read(bitmap_size)
                img = decode_xtg_to_image(bitmap, pw, ph)
            else:
                bitmap_size = ((pw * ph + 7) // 8) * 2
                bitmap = f.read(bitmap_size)
                img = decode_xth_to_image(bitmap, pw, ph)

            out_path = extract_output or f"/tmp/xtc_page_{extract_page}.png"
            img.save(out_path)
            print(f"  ✓ 保存: {out_path}")

    print()
    if ok:
        print("検証結果: ✓ 全チェックパス")
    else:
        print("検証結果: ✗ エラーあり")

    return ok


# ============================================================
# デコード（検証用）
# ============================================================

def decode_xtg_to_image(data: bytes, width: int, height: int) -> Image.Image:
    """XTGビットマップをPIL Imageにデコード（検証・抽出用）"""
    img = Image.new('L', (width, height), 0)
    pixels = img.load()
    bytes_per_row = (width + 7) // 8

    for y in range(height):
        for x in range(width):
            byte_idx = y * bytes_per_row + x // 8
            bit_idx = 7 - (x % 8)
            if data[byte_idx] & (1 << bit_idx):
                pixels[x, y] = 255  # White
            # else: 0 (Black) — already initialized

    return img


def decode_xth_to_image(data: bytes, width: int, height: int) -> Image.Image:
    """XTHビットマップをPIL Imageにデコード（検証・抽出用）

    2ビットプレーン → グレースケール
    0=White(255), 1=DarkGrey(85), 2=LightGrey(170), 3=Black(0)
    """
    plane_size = (width * height + 7) // 8
    plane1 = data[:plane_size]
    plane2 = data[plane_size:plane_size * 2]

    # 2bit値 → 8bitグレースケール
    value_map = {0: 255, 1: 85, 2: 170, 3: 0}

    img = Image.new('L', (width, height), 255)
    pixels = img.load()

    # 列優先（右→左）、8垂直ピクセル/バイト でデコード
    bit_index = 0
    for x in range(width - 1, -1, -1):
        for y in range(height):
            byte_idx = bit_index >> 3
            bit_pos = 7 - (bit_index & 7)

            bit1 = (plane1[byte_idx] >> bit_pos) & 1
            bit2 = (plane2[byte_idx] >> bit_pos) & 1
            val = (bit1 << 1) | bit2

            pixels[x, y] = value_map[val]
            bit_index += 1

    return img


# ============================================================
# CLI
# ============================================================

def main():
    parser = argparse.ArgumentParser(
        description='ZIP/CBZ画像アーカイブ → XTC/XTCH 変換・検証ツール',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
使用例:
  # 1ビットXTC変換（漫画、右→左）
  python3 scripts/zip2xtc.py manga.zip -o manga.xtc --rtl

  # 2ビットXTCH変換
  python3 scripts/zip2xtc.py manga.zip -o manga.xtch --xtch --rtl

  # 既存XTCファイルの検証
  python3 scripts/zip2xtc.py --verify manga.xtc

  # 検証 + ページ抽出
  python3 scripts/zip2xtc.py --verify manga.xtc --extract-page 0 -o page0.png
""")

    parser.add_argument('input', help='入力ZIPファイル（変換モード）またはXTCファイル（検証モード）')
    parser.add_argument('-o', '--output', help='出力ファイルパス')
    parser.add_argument('--xtch', action='store_true', help='2ビットグレースケール(XTCH)で出力')
    parser.add_argument('--rtl', action='store_true', help='右→左読み方向（漫画用）')
    parser.add_argument('--title', default='', help='書籍タイトル')
    parser.add_argument('--author', default='', help='著者名')
    parser.add_argument('--width', type=int, default=DISPLAY_WIDTH, help=f'ページ幅 (default: {DISPLAY_WIDTH})')
    parser.add_argument('--height', type=int, default=DISPLAY_HEIGHT, help=f'ページ高さ (default: {DISPLAY_HEIGHT})')
    parser.add_argument('--no-trim', action='store_true', help='白余白の自動トリミングを無効化')
    parser.add_argument('--verify', action='store_true', help='XTCファイルの検証モード')
    parser.add_argument('--extract-page', type=int, default=-1, help='検証時に指定ページをPNG抽出 (0-based)')

    args = parser.parse_args()

    if args.verify:
        # 検証モード
        success = verify_xtc(
            args.input,
            extract_page=args.extract_page,
            extract_output=args.output or "",
        )
        sys.exit(0 if success else 1)
    else:
        # 変換モード
        if not os.path.exists(args.input):
            print(f"エラー: ファイルが見つかりません: {args.input}", file=sys.stderr)
            sys.exit(1)

        output = args.output
        if not output:
            base = os.path.splitext(args.input)[0]
            output = base + ('.xtch' if args.xtch else '.xtc')

        convert_zip_to_xtc(
            zip_path=args.input,
            output_path=output,
            is_xtch=args.xtch,
            read_direction=READ_RTL if args.rtl else READ_LTR,
            title=args.title,
            author=args.author,
            target_width=args.width,
            target_height=args.height,
            auto_trim=not args.no_trim,
        )


if __name__ == '__main__':
    main()
