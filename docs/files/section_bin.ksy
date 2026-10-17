doc: >
  For storing a parsed and cached version of a section of a book. Written 
  by Section to /.crosspoint/epub_<hash>/section/##.bin

meta:
  id: section_bin
  endian: le

seq: 
  - id: version
    type: u1
    valid:
      any-of: [45, 46, 236]
  - id: font_id
    type: s4
  - id: line_compression
    type: f4
  - id: extra_paragraph_spacing
    type: u1
  - id: paragraph_alignment
    type: u1
    enum: text_align
  - id: viewport_width
    type: u2
  - id: viewport_height
    type: u2
  - id: hyphenation_enabled
    type: u1
  - id: embedded_style
    type: u1
  - id: image_rendering
    type: u1
    enum: image_rendering
  - id: focus_reading_enabled
    type: u1
  - id: page_count
    type: u2
  - id: page_lut_offset
    type: u4
  - id: anchor_map_offset
    type: u4
  - id: paragraph_lut_offset
    type: u4
  - id: list_item_lut_offset
    type: u4
  - id: visible_text_lut_offset
    type: u4
  - id: pages
    type: page
    repeat: expr
    repeat-expr: page_count

instances:
  page_lut:
    pos: _root.page_lut_offset
    type: u4
    repeat: expr
    repeat-expr: _root.page_count
    doc: Absolute file offsets of page data

  anchor_map:
    pos: _root.anchor_map_offset
    type: anchor_map
    if: _root.anchor_map_offset != 0

  paragraph_lut:
    pos: _root.paragraph_lut_offset
    type: paragraph_lut
    if: _root.paragraph_lut_offset != 0

  list_item_index:
    pos: _root.list_item_lut_offset
    type: u2
    repeat: expr
    repeat-expr: paragraph_lut.count
    if: _root.list_item_lut_offset != 0 and _root.paragraph_lut_offset != 0

  visible_text_lut:
    pos: _root.visible_text_lut_offset
    type: u4
    repeat: expr
    repeat-expr: _root.page_count
    if: _root.visible_text_lut_offset != 0

types:
  anchor_map:
    seq:
      - id: count
        type: u2
      - id: anchor_entry
        type: anchor_entry
        repeat: expr
        repeat-expr: count
  
  paragraph_lut:
    seq:
      - id: count
        type: u2
      - id: paragraph_index
        type: u2
        repeat: expr
        repeat-expr: count
  
  anchor_entry:
    seq:
      - id: anchor
        type: u4_prefixed_string
      - id: page
        type: u2
  
  u4_prefixed_string:
    seq:
      - id: len
        type: u4
      - id: value
        type: str
        size: len
        encoding: UTF-8
  
  page:
    seq:
      - id: element_count
        type: u2
      - id: elements
        type: page_element
        repeat: expr
        repeat-expr: element_count
      - id: footnote_count
        type: u2
      - id: footnotes
        type: footnote_entry
        repeat: expr
        repeat-expr: footnote_count
      - id: link_count
        type: u2
      - id: links
        type: link
        repeat: expr
        repeat-expr: link_count
  
  link:
    seq:
      - id: href
        type: str
        encoding: UTF-8
        size: 256
        terminator: 0
        include: false
      - id: x_pos
        type: s2
      - id: y_pos
        type: s2
      - id: width
        type: s2
      - id: height
        type: s2

  page_element:
    seq:
      - id: page_element_type
        type: u1
        enum: page_element_type
      - id: line
        type: page_line
        if: page_element_type == page_element_type::line
      - id: image
        type: page_image
        if: page_element_type == page_element_type::image
      - id: horizontal_rule
        type: page_horizontal_rule
        if: page_element_type == page_element_type::horizontal_rule

  page_line:
    seq:
      - id: x_pos
        type: s2
      - id: y_pos
        type: s2
      - id: block
        type: text_block

  page_image:
    seq:
      - id: x_pos
        type: s2
      - id: y_pos
        type: s2
      - id: image
        type: image_block

  page_horizontal_rule:
    seq:
      - id: x_pos
        type: s2
      - id: y_pos
        type: s2
      - id: width
        type: u2
      - id: thickness
        type: u1

  text_block:
    seq:
      - id: word_count
        type: u2
      - id: has_focus
        type: u1
      - id: text_bytes
        type: u2
        doc: Total size of text[], including one NUL per word
      - id: text_off
        type: u2
        repeat: expr
        repeat-expr: word_count
        doc: "Byte offset of word i's text within text[]"
        if: word_count > 0
      - id: word_x_pos
        type: s2
        repeat: expr
        repeat-expr: word_count
        if: word_count > 0
      - id: word_focus_suffix_x
        type: u2
        repeat: expr
        repeat-expr: word_count
        if: has_focus != 0 and word_count > 0
        doc: Suffix x offset from word start.
      - id: word_style
        type: u1
        enum: word_style
        repeat: expr
        repeat-expr: word_count
        if: word_count > 0
      - id: word_focus_boundary
        type: u1
        repeat: expr
        repeat-expr: word_count
        if: has_focus != 0 and word_count > 0
        doc: "UTF-8 byte boundary between bold prefix and suffix"
      - id: text
        type: strz
        encoding: UTF-8
        include: false
        consume: true
        repeat: expr
        repeat-expr: word_count
        if: word_count > 0
        doc: Array of NUL-terminated UTF-8 words 
      - id: ruby_text
        type: u4_prefixed_string
        repeat: expr
        repeat-expr: word_count
        if: word_count > 0
        doc: One ruby annotation string per word; usually empty
      - id: block_style
        type: block_style
  
  image_block:
    seq:
      - id: image_path
        type: u4_prefixed_string
      - id: src_path
        type: u4_prefixed_string
      - id: width
        type: s2
      - id: height
        type: s2

  block_style:
    seq:
      - id: text_align
        type: u1
        enum: text_align
      - id: text_align_defined
        type: u1
      - id: margin_top
        type: s2
      - id: margin_bottom
        type: s2
      - id: margin_left
        type: s2
      - id: margin_right
        type: s2
      - id: padding_top
        type: s2
      - id: padding_bottom
        type: s2
      - id: padding_left
        type: s2
      - id: padding_right
        type: s2
      - id: text_indent
        type: s2
      - id: text_indent_defined
        type: u1
      - id: is_rtl
        type: u1
      - id: direction_defined
        type: u1

  footnote_entry:
    seq:
      - id: number
        size: 32
        type: str
        encoding: UTF-8
        terminator: 0
        include: false
      - id: href
        size: 256
        type: str
        encoding: UTF-8
        terminator: 0
        include: false

enums: 
  text_align:
    0: justify
    1: left
    2: center
    3: right
    4: none

  image_rendering:
    0: display
    1: placeholder
    2: suppress

  page_element_type:
    1: line
    2: image
    3: horizontal_rule

  word_style:
    0: regular
    1: bold
    2: italic
    3: bold_italic
    4: underline
    8: strikethrough
    16: sup
    32: sub

  