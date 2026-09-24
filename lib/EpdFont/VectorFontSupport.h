#pragma once

// Runtime TTF/OTF rendering (FreeInkFont/FreeType via TtfEpdFont) is a
// PSRAM-boards-only feature. On the no-PSRAM ESP32-C3 devices the engine's
// working set (FreeType face + table state, per-style glyph arenas, resident
// or GPOS font bytes) competes with the reader's section build for the same
// ~380KB of internal DRAM, and the ~110KB of FreeType flash rides along.
// Gating on BOARD_HAS_PSRAM (from the board definition or platformio.ini)
// compiles the whole path out: FreeType never links, .ttf/.otf files are
// invisible to the font registry, and .cpfont SD fonts remain the only
// sideloaded-font route on those boards.
#ifdef BOARD_HAS_PSRAM
#define CROSSPOINT_VECTOR_FONTS 1
#else
#define CROSSPOINT_VECTOR_FONTS 0
#endif
