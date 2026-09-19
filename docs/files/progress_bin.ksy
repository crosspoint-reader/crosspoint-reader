doc: For storing the current ereader progress into a book

meta:
  id: progress_bin
  file-extension: progress.bin
  endian: le

seq:
  - id: spine_index
    type: u2
  - id: page_number
    type: u2
  - id: chapter_total_page_count
    type: u2
    if: _io.size >= 6
  - id: visible_text_offset
    type: u4
    if: _io.size == 10

