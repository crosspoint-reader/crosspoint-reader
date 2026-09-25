#pragma once

// Applied on top of HB_TINY (see library.json).

// Every HarfBuzz allocation goes through the budgeted allocator in
// lib/EpdFont/ComplexShaper.cpp, so a failed allocation degrades shaping
// instead of aborting.
#define hb_malloc_impl crosspointHbMalloc
#define hb_calloc_impl crosspointHbCalloc
#define hb_realloc_impl crosspointHbRealloc
#define hb_free_impl crosspointHbFree

// Drop the per-subtable coverage digests from the lookup accelerators
// (scripts/harfbuzz_patches/0001-drop-subtable-coverage-digests.patch).
// They only let HarfBuzz skip a
// subtable before its own coverage check; without them each subtable costs
// two pointers instead of five words, which is most of HarfBuzz's heap on a
// complex-script face.
// Requires HB_NO_OT_LAYOUT_LOOKUP_CACHE (set by HB_TINY): the cached apply
// path still reads the digest and would not compile.
#define HB_CROSSPOINT_NO_SUBTABLE_DIGEST
