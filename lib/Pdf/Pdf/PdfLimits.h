#pragma once

#include <cstddef>

// Max uncompressed bytes we hold for one page content stream (inflate output + parse buffer).
// Default 96 KiB fits typical text pages on ESP32-class devices (~320 KiB RAM); 200 KiB was too large.
#ifndef PDF_CONTENT_STREAM_MAX
#define PDF_CONTENT_STREAM_MAX (96 * 1024)
#endif

// --- Bounded PDF model (no heap in lib/Pdf): reject / fail closed when exceeded ---

#ifndef PDF_MAX_PATH
#define PDF_MAX_PATH 256
#endif

#ifndef PDF_MAX_OBJECTS
#define PDF_MAX_OBJECTS 16384
#endif

#ifndef PDF_MAX_PAGES
#define PDF_MAX_PAGES 4096
#endif

#ifndef PDF_MAX_OUTLINE_ENTRIES
#define PDF_MAX_OUTLINE_ENTRIES 512
#endif

// Object body read buffer (dual static buffers for nested readAt; see PdfObject.cpp).
#ifndef PDF_OBJECT_BODY_MAX
#define PDF_OBJECT_BODY_MAX 131072
#endif

#ifndef PDF_DICT_VALUE_MAX
#define PDF_DICT_VALUE_MAX 8192
#endif

// Object streams / xref: single reusable static workspace (sequential use only).
#ifndef PDF_LARGE_WORK_BYTES
#define PDF_LARGE_WORK_BYTES (256 * 1024)
#endif

// Inline (ObjStm) cache pool
#ifndef PDF_MAX_INLINE_OBJECTS
#define PDF_MAX_INLINE_OBJECTS 64
#endif

#ifndef PDF_INLINE_DICT_MAX
#define PDF_INLINE_DICT_MAX 2048
#endif

#ifndef PDF_INLINE_STREAM_MAX
#define PDF_INLINE_STREAM_MAX 8192
#endif

// Per-page extracted layout
#ifndef PDF_MAX_TEXT_BLOCKS
#define PDF_MAX_TEXT_BLOCKS 256
#endif

#ifndef PDF_MAX_IMAGES_PER_PAGE
#define PDF_MAX_IMAGES_PER_PAGE 64
#endif

#ifndef PDF_MAX_DRAW_STEPS
#define PDF_MAX_DRAW_STEPS 512
#endif

#ifndef PDF_MAX_TEXT_BLOCK_BYTES
#define PDF_MAX_TEXT_BLOCK_BYTES 4096
#endif

#ifndef PDF_MAX_OUTLINE_TITLE_BYTES
#define PDF_MAX_OUTLINE_TITLE_BYTES 256
#endif

// Content stream operator stack / runs
#ifndef PDF_MAX_OP_STACK
#define PDF_MAX_OP_STACK 128
#endif

#ifndef PDF_MAX_TMP_RUNS
#define PDF_MAX_TMP_RUNS 512
#endif

#ifndef PDF_MAX_TMP_RUN_UTF8
#define PDF_MAX_TMP_RUN_UTF8 2048
#endif

#ifndef PDF_MAX_STACK_TOKEN_BYTES
#define PDF_MAX_STACK_TOKEN_BYTES 8192
#endif

#ifndef PDF_MAX_FONT_CID_MAPS
#define PDF_MAX_FONT_CID_MAPS 16
#endif

#ifndef PDF_MAX_CID_MAP_ENTRIES
#define PDF_MAX_CID_MAP_ENTRIES 384
#endif

// Classic xref merge: one slot per chain depth (see XrefTable.cpp).
#ifndef PDF_MAX_XREF_CHAIN_DEPTH
#define PDF_MAX_XREF_CHAIN_DEPTH 128
#endif

#ifndef PDF_MAX_XREF_UPDATES_PER_SECTION
#define PDF_MAX_XREF_UPDATES_PER_SECTION 256
#endif

inline constexpr size_t pdfContentStreamMaxBytes() { return static_cast<size_t>(PDF_CONTENT_STREAM_MAX); }
