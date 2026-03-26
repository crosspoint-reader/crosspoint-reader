#pragma once

#include <cstddef>

// Production build keeps these buffers small; host tests use larger limits via the stub HAL.
#ifdef HAL_STORAGE_STUB
#ifndef PDF_CONTENT_STREAM_MAX
#define PDF_CONTENT_STREAM_MAX (16 * 1024)
#endif
#else
#ifndef PDF_CONTENT_STREAM_MAX
#define PDF_CONTENT_STREAM_MAX (4 * 1024)
#endif
#endif

// --- Bounded PDF model (no heap in lib/Pdf): reject / fail closed when exceeded ---

#ifndef PDF_MAX_PATH
#define PDF_MAX_PATH 256
#endif

#ifndef PDF_MAX_OBJECTS
#define PDF_MAX_OBJECTS 2048
#endif

#ifndef PDF_MAX_PAGES
#define PDF_MAX_PAGES 64
#endif

#ifndef PDF_MAX_OUTLINE_ENTRIES
#define PDF_MAX_OUTLINE_ENTRIES 64
#endif

// Object body read buffer (dual static buffers for nested readAt; see PdfObject.cpp).
#ifdef HAL_STORAGE_STUB
#ifndef PDF_OBJECT_BODY_MAX
#define PDF_OBJECT_BODY_MAX 16384
#endif
#else
#ifndef PDF_OBJECT_BODY_MAX
#define PDF_OBJECT_BODY_MAX 512
#endif
#endif

#ifdef HAL_STORAGE_STUB
#ifndef PDF_DICT_VALUE_MAX
#define PDF_DICT_VALUE_MAX 256
#endif
#else
#ifndef PDF_DICT_VALUE_MAX
#define PDF_DICT_VALUE_MAX 512
#endif
#endif

// Object streams / xref: single reusable static workspace (sequential use only).
#ifdef HAL_STORAGE_STUB
#ifndef PDF_LARGE_WORK_BYTES
#define PDF_LARGE_WORK_BYTES (4 * 1024)
#endif
#else
#ifndef PDF_LARGE_WORK_BYTES
#define PDF_LARGE_WORK_BYTES 1024
#endif
#endif

// Inline (ObjStm) cache pool
#ifdef HAL_STORAGE_STUB
#ifndef PDF_MAX_INLINE_OBJECTS
#define PDF_MAX_INLINE_OBJECTS 32
#endif
#else
#ifndef PDF_MAX_INLINE_OBJECTS
#define PDF_MAX_INLINE_OBJECTS 8
#endif
#endif

#ifdef HAL_STORAGE_STUB
#ifndef PDF_INLINE_DICT_MAX
#define PDF_INLINE_DICT_MAX 192
#endif
#else
#ifndef PDF_INLINE_DICT_MAX
#define PDF_INLINE_DICT_MAX 256
#endif
#endif

#ifdef HAL_STORAGE_STUB
#ifndef PDF_INLINE_STREAM_MAX
#define PDF_INLINE_STREAM_MAX 192
#endif
#else
#ifndef PDF_INLINE_STREAM_MAX
#define PDF_INLINE_STREAM_MAX 32
#endif
#endif

// Per-page extracted layout
#ifndef PDF_MAX_TEXT_BLOCKS
#define PDF_MAX_TEXT_BLOCKS 64
#endif

#ifndef PDF_MAX_IMAGES_PER_PAGE
#define PDF_MAX_IMAGES_PER_PAGE 4
#endif

#ifndef PDF_MAX_DRAW_STEPS
#define PDF_MAX_DRAW_STEPS 24
#endif

#ifndef PDF_MAX_TEXT_BLOCK_BYTES
#define PDF_MAX_TEXT_BLOCK_BYTES 96
#endif

#ifndef PDF_MAX_OUTLINE_TITLE_BYTES
#define PDF_MAX_OUTLINE_TITLE_BYTES 128
#endif

// Content stream operator stack / runs
#ifndef PDF_MAX_OP_STACK
#define PDF_MAX_OP_STACK 8
#endif

#ifndef PDF_MAX_TMP_RUNS
#define PDF_MAX_TMP_RUNS 8
#endif

#ifndef PDF_MAX_TMP_RUN_UTF8
#define PDF_MAX_TMP_RUN_UTF8 96
#endif

#ifndef PDF_MAX_STACK_TOKEN_BYTES
#define PDF_MAX_STACK_TOKEN_BYTES 96
#endif

#ifndef PDF_MAX_FONT_CID_MAPS
#define PDF_MAX_FONT_CID_MAPS 1
#endif

#ifndef PDF_MAX_CID_MAP_ENTRIES
#define PDF_MAX_CID_MAP_ENTRIES 32
#endif

// Classic xref merge: one slot per chain depth (see XrefTable.cpp).
#ifndef PDF_MAX_XREF_CHAIN_DEPTH
#define PDF_MAX_XREF_CHAIN_DEPTH 4
#endif

#ifndef PDF_MAX_XREF_UPDATES_PER_SECTION
#define PDF_MAX_XREF_UPDATES_PER_SECTION 128
#endif

inline constexpr size_t pdfContentStreamMaxBytes() { return static_cast<size_t>(PDF_CONTENT_STREAM_MAX); }
