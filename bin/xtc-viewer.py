#!/usr/bin/env python3
"""
XTC/XTCH file viewer for CrossPoint Reader e-book format.

Usage: python xtc-viewer.py <file.xtc|file.xtch>
"""

import logging
import struct
import sys
import tkinter as tk
from tkinter import ttk, messagebox
from pathlib import Path
from enum import Enum
from PIL import Image, ImageTk

logging.basicConfig(
    level=logging.DEBUG,
    format="%(asctime)s [%(levelname)s] %(message)s",
    datefmt="%H:%M:%S",
)


class BitDepth(Enum):
    XTG_1BIT = 1
    XTH_2BIT = 2


class XtcFile:
    XTC_MAGIC = 0x00435458
    XTCH_MAGIC = 0x48435458
    XTG_PAGE_MAGIC = 0x00475458
    XTH_PAGE_MAGIC = 0x00485458

    def __init__(self, filepath: str):
        self.filepath = Path(filepath)
        self.bit_depth: BitDepth | None = None
        self.page_count = 0
        self.page_table: list[dict] = []
        self.title = ""
        self._file_handle = None

    def open(self):
        logging.info(f"Opening file: {self.filepath}")
        logging.info(f"File size: {self.filepath.stat().st_size} bytes")
        self._file_handle = open(self.filepath, "rb")
        self._read_header()
        self._read_page_table()
        self._read_title()
        logging.info(f"File opened successfully: {self.page_count} pages, bit_depth={self.bit_depth}")

    def close(self):
        if self._file_handle:
            self._file_handle.close()
            self._file_handle = None

    def _read_header(self):
        self._file_handle.seek(0)
        data = self._file_handle.read(56)
        logging.debug(f"Read {len(data)} bytes for header")
        logging.debug(f"First 16 bytes (hex): {data[:16].hex()}")

        (
            magic,
            ver_maj,
            ver_min,
            page_count,
            flags,
            header_size,
            reserved1,
            toc_offset,
            page_table_offset,
            data_offset,
            reserved2,
            title_offset,
            padding,
        ) = struct.unpack("<IBBHIIIIQQQII", data)

        logging.debug(f"Magic: 0x{magic:08x} (expected XTC=0x{self.XTC_MAGIC:08x}, XTCH=0x{self.XTCH_MAGIC:08x})")
        logging.debug(f"Version: {ver_maj}.{ver_min}")
        logging.debug(f"Page count: {page_count}")
        logging.debug(f"Header size: {header_size}")
        logging.debug(f"Page table offset: {page_table_offset}")
        logging.debug(f"Data offset: {data_offset}")
        logging.debug(f"Title offset: {title_offset}")

        if magic == self.XTC_MAGIC:
            self.bit_depth = BitDepth.XTG_1BIT
            logging.info("Detected XTC (1-bit) format")
        elif magic == self.XTCH_MAGIC:
            self.bit_depth = BitDepth.XTH_2BIT
            logging.info("Detected XTCH (2-bit) format")
        else:
            logging.error(f"Invalid magic number: 0x{magic:08x}")
            raise ValueError(f"Invalid magic number: 0x{magic:08x}")

        self.page_count = page_count
        self._page_table_offset = page_table_offset
        self._title_offset = title_offset

    def _read_page_table(self):
        logging.debug(f"Reading page table at offset {self._page_table_offset}")
        self._file_handle.seek(self._page_table_offset)
        self.page_table = []

        for i in range(self.page_count):
            entry = self._file_handle.read(16)
            offset, size, width, height = struct.unpack("<QIHH", entry)
            self.page_table.append(
                {"index": i, "offset": offset, "size": size, "width": width, "height": height}
            )
            if i < 3 or i == self.page_count - 1:
                logging.debug(f"  Page {i}: offset={offset}, size={size}, {width}x{height}")
            elif i == 3:
                logging.debug(f"  ... ({self.page_count - 4} more pages) ...")

    def _read_title(self):
        if self._title_offset > 0:
            logging.debug(f"Reading title at offset {self._title_offset}")
            self._file_handle.seek(self._title_offset)
            title_bytes = b""
            while True:
                byte = self._file_handle.read(1)
                if not byte or byte == b"\x00":
                    break
                title_bytes += byte
                if len(title_bytes) > 128:
                    break
            self.title = title_bytes.decode("utf-8", errors="replace")
            logging.info(f"Title: '{self.title}'")

    def load_page(self, page_index: int) -> Image.Image:
        if page_index < 0 or page_index >= self.page_count:
            raise IndexError(f"Page {page_index} out of range (0-{self.page_count - 1})")

        page_info = self.page_table[page_index]
        logging.debug(f"Loading page {page_index} from offset {page_info['offset']}")
        self._file_handle.seek(page_info["offset"])

        header = self._file_handle.read(22)
        magic, width, height, color_mode, compression, data_size, md5 = struct.unpack(
            "<IHHBBIQ", header
        )

        logging.debug(f"Page header: magic=0x{magic:08x}, {width}x{height}, compression={compression}, data_size={data_size}")

        expected_magic = (
            self.XTG_PAGE_MAGIC if self.bit_depth == BitDepth.XTG_1BIT else self.XTH_PAGE_MAGIC
        )
        if magic != expected_magic:
            logging.error(f"Invalid page magic: 0x{magic:08x}, expected 0x{expected_magic:08x}")
            raise ValueError(f"Invalid page magic: 0x{magic:08x}, expected 0x{expected_magic:08x}")

        bitmap = self._file_handle.read(data_size)
        logging.debug(f"Read {len(bitmap)} bytes of bitmap data")

        if self.bit_depth == BitDepth.XTG_1BIT:
            return self._decode_1bit(bitmap, width, height)
        else:
            return self._decode_2bit(bitmap, width, height)

    def _decode_1bit(self, bitmap: bytes, width: int, height: int) -> Image.Image:
        """Decode 1-bit monochrome (row-major, MSB first)."""
        row_size = (width + 7) // 8
        img = Image.new("L", (width, height))
        pixels = img.load()

        for y in range(height):
            for x in range(width):
                byte_idx = y * row_size + x // 8
                if byte_idx < len(bitmap):
                    bit_idx = 7 - (x % 8)
                    pixel = (bitmap[byte_idx] >> bit_idx) & 1
                    pixels[x, y] = 255 if pixel else 0

        return img

    def _decode_2bit(self, bitmap: bytes, width: int, height: int) -> Image.Image:
        """Decode 2-bit grayscale (column-major, right-to-left, two bit planes)."""
        plane_size = (width * height + 7) // 8
        plane1 = bitmap[:plane_size]
        plane2 = bitmap[plane_size : plane_size * 2]
        col_bytes = (height + 7) // 8

        img = Image.new("L", (width, height))
        pixels = img.load()

        for y in range(height):
            for x in range(width):
                col_idx = width - 1 - x
                byte_in_col = y // 8
                bit_in_byte = 7 - (y % 8)
                byte_offset = col_idx * col_bytes + byte_in_col

                if byte_offset < len(plane1) and byte_offset < len(plane2):
                    bit1 = (plane1[byte_offset] >> bit_in_byte) & 1
                    bit2 = (plane2[byte_offset] >> bit_in_byte) & 1
                    pixel_value = (bit1 << 1) | bit2
                    grayscale = (3 - pixel_value) * 85
                    pixels[x, y] = grayscale

        return img


class XtcViewer:
    def __init__(self, filepath: str):
        self.xtc = XtcFile(filepath)
        self.current_page = 0
        self.photo_image = None
        self.landscape = False
        self.native_size = False  # 1:1 pixel mapping to X4 display (480x800)

        self.root = tk.Tk()
        self.root.title(f"XTC Viewer - {Path(filepath).name}")
        self.root.configure(bg="#2d2d2d")

        self._setup_ui()
        self._bind_keys()

    def _setup_ui(self):
        main_frame = ttk.Frame(self.root, padding=10)
        main_frame.pack(fill=tk.BOTH, expand=True)

        style = ttk.Style()
        style.configure("TButton", padding=6)
        style.configure("TLabel", background="#2d2d2d", foreground="white")

        self.info_label = ttk.Label(main_frame, text="Loading...", font=("Arial", 10))
        self.info_label.pack(pady=(0, 10))

        canvas_frame = ttk.Frame(main_frame)
        canvas_frame.pack(fill=tk.BOTH, expand=True)

        self.canvas = tk.Canvas(canvas_frame, bg="#1a1a1a", highlightthickness=0)
        self.canvas.pack(fill=tk.BOTH, expand=True)

        button_frame = ttk.Frame(main_frame)
        button_frame.pack(pady=(10, 0))

        self.prev_button = ttk.Button(button_frame, text="< Previous", command=self.prev_page)
        self.prev_button.pack(side=tk.LEFT, padx=5)

        self.page_label = ttk.Label(button_frame, text="Page 0 / 0", font=("Arial", 11))
        self.page_label.pack(side=tk.LEFT, padx=20)

        self.next_button = ttk.Button(button_frame, text="Next >", command=self.next_page)
        self.next_button.pack(side=tk.LEFT, padx=5)

        self.rotate_button = ttk.Button(button_frame, text="⟳ Landscape", command=self.toggle_orientation)
        self.rotate_button.pack(side=tk.LEFT, padx=(20, 5))

        self.native_button = ttk.Button(button_frame, text="1:1 X4", command=self.toggle_native_size)
        self.native_button.pack(side=tk.LEFT, padx=5)

    def _bind_keys(self):
        self.root.bind("<Left>", lambda e: self.prev_page())
        self.root.bind("<Right>", lambda e: self.next_page())
        self.root.bind("<Up>", lambda e: self.prev_page())
        self.root.bind("<Down>", lambda e: self.next_page())
        self.root.bind("<Prior>", lambda e: self.prev_page())
        self.root.bind("<Next>", lambda e: self.next_page())
        self.root.bind("<space>", lambda e: self.next_page())
        self.root.bind("<Escape>", lambda e: self.root.quit())
        self.root.bind("r", lambda e: self.toggle_orientation())
        self.root.bind("R", lambda e: self.toggle_orientation())
        self.root.bind("1", lambda e: self.toggle_native_size())
        self.canvas.bind("<Configure>", lambda e: self._display_page())

    def run(self):
        try:
            logging.info("Starting viewer...")
            self.xtc.open()
            title = self.xtc.title or Path(self.xtc.filepath).stem
            bit_info = "1-bit" if self.xtc.bit_depth == BitDepth.XTG_1BIT else "2-bit"
            self.info_label.config(text=f"{title} ({bit_info}, {self.xtc.page_count} pages)")
            logging.info(f"Updated info label: {title} ({bit_info}, {self.xtc.page_count} pages)")

            if self.xtc.page_count > 0:
                page_info = self.xtc.page_table[0]
                window_width = min(page_info["width"] + 40, 800)
                window_height = min(page_info["height"] + 120, 950)
                self.root.geometry(f"{window_width}x{window_height}")
                logging.info(f"Set window geometry to {window_width}x{window_height}")

            self._display_page()
            self._update_buttons()
            logging.info("Entering main loop")
            self.root.mainloop()
        except Exception as e:
            logging.exception(f"Error in viewer: {e}")
            messagebox.showerror("Error", str(e))
            raise
        finally:
            self.xtc.close()

    def _display_page(self):
        if self.xtc.page_count == 0:
            logging.warning("No pages to display")
            return

        try:
            logging.debug(f"Displaying page {self.current_page}")
            img = self.xtc.load_page(self.current_page)
            logging.debug(f"Loaded image: {img.width}x{img.height}")

            if self.landscape:
                img = img.rotate(90, expand=True)
                logging.debug(f"Rotated to landscape: {img.width}x{img.height}")

            canvas_width = self.canvas.winfo_width()
            canvas_height = self.canvas.winfo_height()
            logging.debug(f"Canvas size: {canvas_width}x{canvas_height}")

            if self.native_size:
                # 1:1 pixel mapping - resize window to fit native image size
                logging.debug(f"Native size mode: {img.width}x{img.height}")
            elif canvas_width > 1 and canvas_height > 1:
                scale_x = canvas_width / img.width
                scale_y = canvas_height / img.height
                scale = min(scale_x, scale_y, 1.0)

                if scale < 1.0:
                    new_width = int(img.width * scale)
                    new_height = int(img.height * scale)
                    img = img.resize((new_width, new_height), Image.Resampling.LANCZOS)
                    logging.debug(f"Scaled image to {new_width}x{new_height}")

            self.photo_image = ImageTk.PhotoImage(img)

            self.canvas.delete("all")
            x = max(0, (canvas_width - img.width) // 2)
            y = max(0, (canvas_height - img.height) // 2)
            self.canvas.create_image(x, y, anchor=tk.NW, image=self.photo_image)
            logging.debug(f"Image placed at ({x}, {y})")

        except Exception as e:
            logging.exception(f"Error displaying page: {e}")
            self.canvas.delete("all")
            self.canvas.create_text(
                self.canvas.winfo_width() // 2,
                self.canvas.winfo_height() // 2,
                text=f"Error loading page: {e}",
                fill="red",
            )

    def _update_buttons(self):
        self.page_label.config(text=f"Page {self.current_page + 1} / {self.xtc.page_count}")
        self.prev_button.config(state=tk.NORMAL if self.current_page > 0 else tk.DISABLED)
        self.next_button.config(
            state=tk.NORMAL if self.current_page < self.xtc.page_count - 1 else tk.DISABLED
        )

    def prev_page(self):
        if self.current_page > 0:
            self.current_page -= 1
            self._display_page()
            self._update_buttons()

    def next_page(self):
        if self.current_page < self.xtc.page_count - 1:
            self.current_page += 1
            self._display_page()
            self._update_buttons()

    def toggle_orientation(self):
        self.landscape = not self.landscape
        if self.landscape:
            self.rotate_button.config(text="⟳ Portrait")
            logging.info("Switched to landscape mode")
        else:
            self.rotate_button.config(text="⟳ Landscape")
            logging.info("Switched to portrait mode")
        # Resize window if in native size mode
        if self.native_size:
            if self.landscape:
                self.root.geometry(f"{800 + 40}x{480 + 120}")
            else:
                self.root.geometry(f"{480 + 40}x{800 + 120}")
        self._display_page()

    def toggle_native_size(self):
        self.native_size = not self.native_size
        if self.native_size:
            self.native_button.config(text="Fit")
            # Resize window to native X4 display size + UI chrome
            if self.landscape:
                self.root.geometry(f"{800 + 40}x{480 + 120}")
            else:
                self.root.geometry(f"{480 + 40}x{800 + 120}")
            logging.info("Switched to 1:1 native X4 size (480x800)")
        else:
            self.native_button.config(text="1:1 X4")
            logging.info("Switched to fit-to-window mode")
        self._display_page()


def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <file.xtc|file.xtch>")
        sys.exit(1)

    filepath = sys.argv[1]
    if not Path(filepath).exists():
        print(f"Error: File not found: {filepath}")
        sys.exit(1)

    viewer = XtcViewer(filepath)
    viewer.run()


if __name__ == "__main__":
    main()
