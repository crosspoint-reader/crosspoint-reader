
doc: >
  Caches high-level details on a single book, written by BookMetadataCache to 
  /.crosspoint/epub_<hash>/book.bin.
  
meta:
  id: book_bin
  endian: le

seq:
  - id: version
    contents: [10]
  - id: lut_offset
    type: u4
    doc: Offset to lookup tables
  - id: spine_count
    type: u2
  - id: toc_count
    type: u2
  - id: metadata
    type: metadata
  - id: spine_lut
    type: u4
    repeat: expr
    repeat-expr: spine_count
    doc: Spine entry offsets
  - id: toc_lut
    type: u4
    repeat: expr
    repeat-expr: toc_count
    doc: TOC entry offsets
  - id: spines
    type: spine_entry
    repeat: expr
    repeat-expr: spine_count
  - id: toc
    type: toc_entry
    repeat: expr
    repeat-expr: toc_count

types:
  u4_prefixed_string:
    seq:
      - id: len
        type: u4
      - id: value
        type: str
        size: len
        encoding: UTF-8

  metadata:
    seq:
      - id: title
        type: u4_prefixed_string
        doc: Book title
      - id: author
        type: u4_prefixed_string
        doc: Book author
      - id: language
        type: u4_prefixed_string
        doc: Book language code
      - id: cover_item_href
        type: u4_prefixed_string
        doc: Path to cover image
      - id: text_reference_href
        type: u4_prefixed_string
        doc: Path to guided first text reference

  spine_entry:
    seq:
      - id: href
        type: u4_prefixed_string
        doc: Resource path
      - id: cumulative_size
        type: u4
        doc: Cumulative uncompressed spine size through this entry
      - id: toc_index
        type: s2
        doc: Index into TOC or inherited/previous TOC index when no direct entry exists

  toc_entry:
    seq:
      - id: title
        type: u4_prefixed_string
        doc: Chapter/section title
      - id: href
        type: u4_prefixed_string
        doc: Resource path
      - id: anchor
        type: u4_prefixed_string
        doc: Fragment identifier
      - id: level
        type: u1
        doc: Nesting level
      - id: spine_index
        type: s2
        doc: Index into spine (-1 if none)