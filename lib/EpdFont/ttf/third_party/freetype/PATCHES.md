# FreeType smooth rasterizer (vendored)

`ftgrays.c`, `ftgrays.h` and `ftimage.h` are FreeType 2.13.3's anti-aliasing rasterizer, the version the
SDK vendors under `freeink-sdk/libs/font/FreeInkFont/third_party/freetype`. The TTF engine
(`lib/EpdFont/ttf/TtfRasterizer.cpp`) uses it to turn glyph outlines into coverage spans. It is built
standalone: nothing else from FreeType is compiled into the firmware.

Licence: the FreeType License, `FTL.TXT` in this folder. It requires crediting the FreeType project in
the documentation of any product that ships this code; that credit belongs in the user guide's TTF
section.

## Regenerating

    python scripts/vendor_ftgrays.py

The script copies the four files from the SDK with LF line endings and applies the patches below. Each
patch must match its anchor exactly once, so a FreeType update that moves the code fails in the script.

## Patches (each marked `PATCH(witchhunt)` in the source)

1. **Standalone build.** `ftgrays.c` defines `STANDALONE_` itself, so PlatformIO and CMake compile it
   directly with no extra flags.
2. **`FALL_THROUGH`.** FreeType's `compiler-macros.h` defines it; the standalone build has none. Defined
   as `__attribute__((fallthrough))` on GCC 7+ and clang.
3. **FreeType's 64-bit types.** `FT_INT64`, `FT_UINT64`, `FT_Int64`, `FT_UInt64` and `FT_UInt`, as a full
   FreeType build defines them in `integer-types.h` and `fttypes.h`. Without them the standalone build takes
   an older line renderer and conic flattening than every full FreeType build: about 1-3% of glyphs at text
   sizes (more at heading sizes) then differ from FreeType by one gray level on one pixel, and the output
   depends on the width of `long`, so
   host and device disagree. With them the output is FreeType's, bit for bit, on every platform. The cost on
   the C3 is the integer helpers `__divdi3` and `__ashldi3` and about 16% more time per glyph (363 against
   313 µs for text glyphs at 14 pt on the X3). Decided 2026-10-14; the spec's Appendix A has the A/B.
4. **The cell pool comes from the caller.** Upstream keeps the pool in a stack array in
   `gray_convert_glyph` (the SDK's copy caps it at 4 KB; still a 4,400 B frame on the C3). Here the pool
   is passed to `raster_reset(raster, pool, bytes)`, kept in the raster object, and `gray_convert_glyph`
   works in it. The pool must be aligned for a cell and at least 2,048 B; without one, `raster_render`
   returns `Invalid_Argument`. The pool size changes only how many bands a glyph is split into, never
   the output. `FT_MAX_GRAY_POOL` and the SDK's pool cap are removed.
5. **`tags` type.** `FT_Outline_Decompose` declared `char* tags` against `unsigned char*` in `ftimage.h`
   (a sign warning in the standalone build).
6. **Renamed raster.** `ft_grays_raster` is `ttf_grays_raster`, so it cannot clash with a full FreeType
   linked into the same program (the host tests link one for the comparison).

The standalone raster object is a single static (upstream): renders must not overlap. The TTF engine
renders only under `RenderLock`.
