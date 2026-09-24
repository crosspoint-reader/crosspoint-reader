#pragma once

// Host-test stub. The real header self-gates on VectorFontSupport.h's
// CROSSPOINT_VECTOR_FONTS (PSRAM boards only); host tests build with the
// gate off, so FontCacheManager.cpp compiles none of its TTF dispatch and
// only needs the macro and the forward-declared pointer type.
#define CROSSPOINT_VECTOR_FONTS 0

class TtfEpdFont;
