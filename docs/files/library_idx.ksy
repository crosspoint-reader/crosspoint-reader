doc: >
  Maintains a database of all books on the device for the purposes of
  sorting and searching. Written by LibraryBuilder to /.crosspoint/library.idx

meta:
  id: clix_format
  endian: le

seq:
  - id: header
    type: clix_header

instances:
  folders:
    pos: _root.header.folder_start
    size: _root.header.folder_len
    type: folder_section
    doc: List of folders in which books may live.
    
  records:
    pos: _root.header.record_start
    size: _root.header.book_count * 128
    type: record_section
    doc: List of entries in the library. Each entry is exactly 128 bytes.
    
  permutations:
    pos: _root.header.perm_start
    size: _root.header.book_count * 4 # 2 fields * 2 bytes * book_count
    type: permutation_section
    doc: Two separate permutations of the indices in the records list.
    
  name_section:
    pos: _root.header.name_start
    size: _root.header.name_len
    type: name_section_raw
    doc: Raw byte container wrapping the collective pool of name strings.

types:
  clix_header:
    seq:
      - id: magic
        contents: "CLX1" 
      - id: format_version
        contents: [2]
      - id: fold_version
        contents: [3]
      - id: flags
        type: clix_flags
      - id: metadata_enabled
        type: b1
      - id: book_count
        type: u2
      - id: folder_count
        type: u2
        doc: Number of distinct folders in which books live
      - id: next_first_seen
        type: u2
      - id: padding_1
        type: u2
      - id: folder_start
        type: u4
        doc: Offset into file at which the Folder section starts.
      - id: folder_len
        type: u4
        doc: Total size of the meaningful data in the folder section.
      - id: record_start
        type: u4
        doc: Offset into file at which the ClixRecords start.
      - id: perm_start
        type: u4
      - id: name_start
        type: u4
      - id: name_len
        type: u4
      - id: self_size
        type: u4
      - id: reserved
        size: 20

    types:
      clix_flags:
        meta:
          bit-endian: le
        seq:
          - id: ranks_degraded
            type: b1
          - id: dedup_degraded
            type: b1
          - id: reserved
            type: b6

  folder_section:
    seq:
      - id: entries
        type: u1_prefixed_string
        repeat: expr
        repeat-expr: _root.header.folder_count

  u1_prefixed_string:
    seq:
      - id: len
        type: u1
      - id: value
        type: str
        size: len
        encoding: UTF-8

  record_section:
    seq:
      - id: entries
        type: clix_record
        repeat: expr
        repeat-expr: _root.header.book_count

  clix_record:
    seq:
      - id: name_offset
        doc: Offset from nameStart into the per-record name blob
        type: u4
      - id: file_size
        doc: Captured while the dirent was open, part of the identity
        type: u4 
      - id: first_seen
        type: u2
      - id: folder_id
        type: u2
        doc: Index into the array of entries in the folder_section.
      - id: name_len
        type: u1
        doc: Length of the name string in the name section
      - id: fold_len
        type: u1
      - id: author_key_len
        type: u1
      - id: metadata_status
        type: u1
        enum: metadata_status
      - id: fold
        type: str
        encoding: UTF-8
        size: fold_len
      - id: fold_padding
        size: 96 - fold_len
      - id: author_key
        type: str
        encoding: UTF-8
        size: author_key_len
      - id: author_key_padding
        size: 12 - author_key_len
      - id: modification_time
        type: u4

    instances:
      name:
        io: _root.name_section._io
        pos: name_offset
        type: name_blob
  
      folder:
        value: _root.folders.entries[folder_id]

    types:
      name_blob:
        seq:
          - id: path_hash
            doc: FNV-1a fingerprint of the complete path
            type: u8
          - id: name
            type: str
            encoding: UTF-8
            size: _parent.name_len
          - id: canonical_author_len
            type: u1
          - id: canonical_author
            type: str
            size: canonical_author_len
            encoding: UTF-8
          - id: title_len
            type: u1
          - id: title
            type: str
            size: title_len
            encoding: UTF-8
          - id: author_len
            type: u1
          - id: author
            type: str
            size: author_len
            encoding: UTF-8

  permutation_section:
    seq:
      - id: author_order
        type: u2
        repeat: expr
        repeat-expr: _root.header.book_count
        doc: List of book indices, permuted to be sorted by the Author
      - id: arrival_order
        type: u2
        repeat: expr
        repeat-expr: _root.header.book_count
        doc: List of book indices, permuted to be sorted by the time of arrival

  name_section_raw:
    seq:
      - id: raw_data
        size-eos: true

enums:
  metadata_status:
    0: not_attempted
    1: extracted
    2: failed
