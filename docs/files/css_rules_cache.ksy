doc: >
  Stores a condensed and simplified representation of the CSS rules/styles
  applied to a book. Written by CssParser to /.crosspoint/epub_<hash>/css_rules.cache

meta:
  id: css_rules_cache
  endian: le

seq:
  - id: version
    contents: [12]
  - id: is_complete
    type: u1
  - id: rule_count
    type: u2
  - id: rules
    type: rule
    repeat: expr
    repeat-expr: rule_count

types:
  rule:
    seq:
      - id: selector_len
        type: u2
      - id: selector
        type: str
        size: selector_len
        encoding: UTF-8
      - id: style_wire
        type: style_wire

  style_wire:
    seq:
      - id: text_align
        type: u1
        enum: text_align
      - id: font_style
        type: u1
        enum: font_style
      - id: font_weight
        type: u1
        enum: font_weight
      - id: text_decoration
        type: text_decoration
      - id: text_direction
        type: u1
        enum: text_direction
      - id: text_indent
        type: css_len
      - id: margin_top
        type: css_len
      - id: margin_bottom
        type: css_len
      - id: margin_left
        type: css_len
      - id: margin_right
        type: css_len
      - id: padding_top
        type: css_len
      - id: padding_bottom
        type: css_len
      - id: padding_left
        type: css_len
      - id: padding_right
        type: css_len
      - id: image_height
        type: css_len
      - id: image_width
        type: css_len
      - id: display
        type: u1
        enum: display
      - id: vertical_align
        type: u1
        enum: vertical_align
      - id: list_style_type
        type: u1
        enum: list_style_type
      - id: defined_bits
        type: defined_bits
      
  text_decoration:
    seq:
      - id: underline
        type: b1
      - id: strikethrough
        type: b1
      - id: padding
        type: b6

  css_len:
    seq:
      - id: value
        type: f4
      - id: unit
        type: u1
        enum: unit

  defined_bits:
    seq:
      - id: text_align_defined
        type: b1
      - id: font_style_defined
        type: b1
      - id: font_weight_defined
        type: b1
      - id: text_decoration_defined
        type: b1
      - id: text_indent_defined
        type: b1
      - id: margin_top_defined
        type: b1
      - id: margin_bottom_defined
        type: b1
      - id: margin_left_defined
        type: b1
      - id: margin_right_defined
        type: b1
      - id: padding_top_defined
        type: b1
      - id: padding_bottom_defined
        type: b1
      - id: padding_left_defined
        type: b1
      - id: padding_right_defined
        type: b1
      - id: image_height_defined
        type: b1
      - id: image_width_defined
        type: b1
      - id: display_defined
        type: b1
      - id: direction_defined
        type: b1
      - id: vertical_align_defined
        type: b1
      - id: list_style_type_defined
        type: b1
      - id: padding
        type: b13

enums: 
  text_align:
    0: justify
    1: left
    2: center
    3: right
    4: none

  font_style:
    0: normal
    1: italic

  font_weight:
    0: normal
    1: bold

  display:
    0: block
    1: none

  vertical_align:
    0: baseline
    1: super
    2: sub

  list_style_type:
    0: disc
    1: none

  unit:
    0: pixels
    1: em
    2: rem
    3: points
    4: percent

  text_direction:
    0: ltr
    1: rtl

  