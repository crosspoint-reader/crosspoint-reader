# Quiz format v1

Normative physical `.quiz` v1 contract shared by the host converter and firmware reader.

## Global rules
- Little-endian integers only.
- Readers MUST decode byte arrays manually; struct casts are forbidden.
- Maximum file size: 64 MiB.
- Text fields are UTF-8, MUST NOT contain embedded NUL, and MAY contain line breaks.
- Limits: title 1-96 bytes, questions 1-10,000, prompt 1-1,024 bytes, choices 2-6, choice text 1-384 bytes, explanation 0-1,024 bytes.
- File layout is exactly `header -> index -> records` with no gaps.
- `declared_file_size` MUST equal the exact physical file size.
- `deck_identity` and `revision_identity` MUST both be non-zero 16-byte values.

## Header (176 bytes)
| Offset | Size | Field | Rules |
|---|---:|---|---|
| 0 | 8 | magic | `47 51 55 49 5A 0D 0A 1A` (`GQUIZ` + `CR LF SUB`) |
| 8 | 2 | version | `1` |
| 10 | 2 | header_size | `176` |
| 12 | 4 | flags | `0` |
| 16 | 4 | declared_file_size | Exact file size in bytes |
| 20 | 4 | question_count | `1..10000` |
| 24 | 4 | index_offset | `176` |
| 28 | 4 | index_bytes | `question_count * 8` |
| 32 | 4 | records_offset | `176 + index_bytes` |
| 36 | 2 | index_entry_size | `8` |
| 38 | 2 | title_length | `1..96` |
| 40 | 16 | deck_identity | Non-zero stable deck identity |
| 56 | 16 | revision_identity | Non-zero content/revision identity |
| 72 | 96 | title_storage | First `title_length` bytes are title UTF-8; remaining bytes are zero |
| 168 | 8 | reserved | All zero |

Reject the file when any fixed field differs, title padding/reserved bytes are non-zero, the identities are zero, or `declared_file_size` does not exactly match the physical file size.

## Index
Each 8-byte entry is `record_offset:u32, record_bytes:u32`.

Entries are ordered by zero-based ordinal. `record_offset` is an absolute file offset. Records MUST be contiguous, strictly non-overlapping, start at `records_offset`, and fill the remainder of the file exactly.

## Question record
Each question starts with a fixed 24-byte descriptor followed by payload bytes.

| Offset | Size | Field | Rules |
|---|---:|---|---|
| 0 | 2 | record_header_size | `24` |
| 2 | 1 | choice_count | `2..6` |
| 3 | 1 | correct_choice | `0 <= value < choice_count` |
| 4 | 2 | prompt_length | `1..1024` |
| 6 | 2 | explanation_length | `0..1024` |
| 8 | 12 | choice_length[6] | First `choice_count` entries are `1..384`; remaining entries are zero |
| 20 | 4 | payload_length | Exact sum of prompt + all active choices + explanation |

Payload order is:
1. prompt bytes
2. choice bytes in order
3. explanation bytes

`record_bytes` from the index MUST equal `24 + payload_length`.

## Reader expectations
- Validate magic, version, file-size limit, fixed header fields, exact `declared_file_size`, identities, and zero padding before use.
- Stream-validate the full index before session start.
- Validate each question descriptor, UTF-8 field, and payload arithmetic before presentation.
- Re-read only the selected index entry and record when loading one question; do not retain the full deck or full index in RAM.
