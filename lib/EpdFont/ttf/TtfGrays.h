#pragma once

// The vendored FreeType rasterizer (third_party/freetype/PATCHES.md), for the one .cpp that drives it.
// ftimage.h and ftgrays.h expect the macros that ftgrays.c defines for itself before it includes them.
// Never include this from a header: ftimage.h's types would clash with a full FreeType's in a program that
// links both, as the host tests do.
#ifndef STANDALONE_
#define STANDALONE_
#endif
#ifndef FT_BEGIN_HEADER
#define FT_BEGIN_HEADER
#define FT_END_HEADER
#endif
#ifndef FT_STATIC_BYTE_CAST
#define FT_STATIC_BYTE_CAST(type, var) (type)(unsigned char)(var)
#endif

#include "third_party/freetype/ftgrays.h"
