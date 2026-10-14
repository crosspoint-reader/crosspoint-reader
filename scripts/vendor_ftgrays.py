"""Vendor FreeType's smooth rasterizer (ftgrays.c) for the TTF engine, standalone and patched.

Copies src/smooth/ftgrays.c, src/smooth/ftgrays.h, include/freetype/ftimage.h and FTL.TXT from the
SDK's FreeType (freeink-sdk/libs/font/FreeInkFont/third_party/freetype, FreeType 2.13.3) into
lib/EpdFont/ttf/third_party/freetype/, with LF line endings, and applies the patches listed in
PATCHES.md there. Every patched spot is marked PATCH(witchhunt).

Each patch replaces text that must occur exactly once in the source, so a FreeType update that moves
the code fails here instead of producing a silently different rasterizer. Run from the repository
root:

    python scripts/vendor_ftgrays.py
"""

import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC = ROOT / "freeink-sdk/libs/font/FreeInkFont/third_party/freetype"
DST = ROOT / "lib/EpdFont/ttf/third_party/freetype"

HEADER_NOTE = """\

/* PATCH(witchhunt): FreeType 2.13.3 src/smooth/ftgrays.c, built standalone for the TTF engine in */
/* lib/EpdFont/ttf.  Regenerate with scripts/vendor_ftgrays.py; the patches are listed in        */
/* PATCHES.md next to this file.                                                                 */
#define STANDALONE_
"""

FALL_THROUGH = """\
#define ft_jmp_buf  jmp_buf

  /* PATCH(witchhunt): FreeType's compiler-macros.h defines this; a standalone build has none. */
#ifndef FALL_THROUGH
#if ( defined( __GNUC__ ) && __GNUC__ >= 7 ) || defined( __clang__ )
#define FALL_THROUGH  __attribute__(( fallthrough ))
#else
#define FALL_THROUGH  ( (void)0 )
#endif
#endif
"""

POOL_CAP_OLD = """\
  /* FreeInkFont: the pool below lives on the STACK of the rendering task
   * (`gray_convert_glyph`), so it must fit an embedded FreeRTOS task stack.
   * The cap lives here rather than only in ftoption.h because build systems
   * cannot scan FreeType's `#include FT_CONFIG_OPTIONS_H` macro indirection,
   * so ftoption.h edits do not invalidate compiled objects.  A smaller pool
   * only makes the rasterizer subdivide into more bands; output is identical.
   */
#undef  FT_RENDER_POOL_SIZE
#define FT_RENDER_POOL_SIZE  4096L

  /* maximum number of gray cells in the buffer */
#if FT_RENDER_POOL_SIZE > 2048
#define FT_MAX_GRAY_POOL  ( FT_RENDER_POOL_SIZE / sizeof ( TCell ) )
#else
#define FT_MAX_GRAY_POOL  ( 2048 / sizeof ( TCell ) )
#endif
"""

POOL_CAP_NEW = """\
  /* PATCH(witchhunt): no FT_MAX_GRAY_POOL; the cell pool is the caller's (gray_raster_reset). */
  /* A pool must hold at least this many bytes, the smallest the upstream code allows.         */
#define GRAY_MIN_POOL_BYTES  2048
"""

RASTER_OLD = """\
  typedef struct gray_TRaster_
  {
    void*  memory;

  } gray_TRaster, *gray_PRaster;
"""

RASTER_NEW = """\
  typedef struct gray_TRaster_
  {
    void*  memory;
    void*  pool;       /* PATCH(witchhunt): the cell pool given to raster_reset */
    size_t pool_size;  /* PATCH(witchhunt): its size in bytes                   */

  } gray_TRaster, *gray_PRaster;
"""

CONVERT_OLD = """\
  static int
  gray_convert_glyph( RAS_ARG )
  {
    TCell    buffer[FT_MAX_GRAY_POOL];
    size_t   height = (size_t)( ras.cbox.yMax - ras.cbox.yMin );
    size_t   n = FT_MAX_GRAY_POOL / 8;
    TCoord   y;
    TCoord   bands[32];  /* enough to accommodate bisections */
    TCoord*  band;

    int  continued = 0;


    /* Initialize the null cell at the end of the poll. */
    ras.cell_null        = buffer + FT_MAX_GRAY_POOL - 1;
"""

CONVERT_NEW = """\
  static int
  gray_convert_glyph( RAS_ARG_ gray_PRaster  raster )
  {
    /* PATCH(witchhunt): the cells live in the caller's pool, not in a stack array */
    TCell*   buffer = (TCell*)raster->pool;
    size_t   cells  = raster->pool_size / sizeof ( TCell );
    size_t   height = (size_t)( ras.cbox.yMax - ras.cbox.yMin );
    size_t   n = cells / 8;
    TCoord   y;
    TCoord   bands[32];  /* enough to accommodate bisections */
    TCoord*  band;

    int  continued = 0;


    if ( !buffer || raster->pool_size < GRAY_MIN_POOL_BYTES )
      return FT_THROW( Invalid_Argument );

    /* Initialize the null cell at the end of the poll. */
    ras.cell_null        = buffer + cells - 1;
"""

CALL_OLD = """\
    return gray_convert_glyph( RAS_VAR );
"""

CALL_NEW = """\
    return gray_convert_glyph( RAS_VAR_ (gray_PRaster)raster );
"""

RESET_OLD = """\
  static void
  gray_raster_reset( FT_Raster       raster,
                     unsigned char*  pool_base,
                     unsigned long   pool_size )
  {
    FT_UNUSED( raster );
    FT_UNUSED( pool_base );
    FT_UNUSED( pool_size );
  }
"""

RESET_NEW = """\
  static void
  gray_raster_reset( FT_Raster       raster,
                     unsigned char*  pool_base,
                     unsigned long   pool_size )
  {
    /* PATCH(witchhunt): keep the caller's cell pool; it must be aligned for a TCell */
    gray_PRaster  rast = (gray_PRaster)raster;


    if ( rast )
    {
      rast->pool      = pool_base;
      rast->pool_size = pool_base ? (size_t)pool_size : 0;
    }
  }
"""

INT64_OLD = "#define FT_ULONG_MAX  ULONG_MAX\n"
INT64_NEW = """\
#define FT_ULONG_MAX  ULONG_MAX

  /* PATCH(witchhunt): the 64-bit types a full FreeType build defines, so this build takes the same */
  /* code paths (64-bit line divisions, DDA conics) and gives FreeType's output on every platform.  */
#define FT_INT64   long long int
#define FT_UINT64  unsigned long long int
  typedef FT_INT64   FT_Int64;
  typedef FT_UINT64  FT_UInt64;
  typedef unsigned int  FT_UInt;
"""

TAGS_OLD ="    char*       tags;\n"
TAGS_NEW = "    unsigned char*  tags;  /* PATCH(witchhunt): ftimage.h's type (was char*) */\n"

RENAME_OLD = "ft_grays_raster"
RENAME_NEW = "ttf_grays_raster"


def read_lf(path: pathlib.Path) -> str:
    return path.read_bytes().decode("utf-8").replace("\r\n", "\n")


def replace_once(text: str, old: str, new: str, what: str) -> str:
    count = text.count(old)
    if count != 1:
        sys.exit(f"vendor_ftgrays: {what}: expected the anchor once, found it {count} times")
    return text.replace(old, new)


def patch_ftgrays(text: str) -> str:
    # The note and STANDALONE_ go after the licence block, before any #include or #ifdef.
    licence_end = text.index("*/\n") + len("*/\n")
    text = text[:licence_end] + HEADER_NOTE + text[licence_end:]
    text = replace_once(text, "#define ft_jmp_buf  jmp_buf\n", FALL_THROUGH, "FALL_THROUGH shim")
    text = replace_once(text, INT64_OLD, INT64_NEW, "64-bit types")
    text = replace_once(text, POOL_CAP_OLD, POOL_CAP_NEW, "pool cap")
    text = replace_once(text, RASTER_OLD, RASTER_NEW, "raster object")
    text = replace_once(text, CONVERT_OLD, CONVERT_NEW, "gray_convert_glyph")
    text = replace_once(text, CALL_OLD, CALL_NEW, "gray_convert_glyph call")
    text = replace_once(text, RESET_OLD, RESET_NEW, "gray_raster_reset")
    text = replace_once(text, TAGS_OLD, TAGS_NEW, "outline tags type")
    text = replace_once(text, RENAME_OLD + ",\n", RENAME_NEW + ",\n", "raster funcs name")
    return text


def patch_ftgrays_h(text: str) -> str:
    return replace_once(text, RENAME_OLD + ";", RENAME_NEW + ";  /* PATCH(witchhunt): renamed */",
                        "ftgrays.h name")


def main() -> None:
    DST.mkdir(parents=True, exist_ok=True)
    files = {
        "ftgrays.c": patch_ftgrays(read_lf(SRC / "src/smooth/ftgrays.c")),
        "ftgrays.h": patch_ftgrays_h(read_lf(SRC / "src/smooth/ftgrays.h")),
        "ftimage.h": read_lf(SRC / "include/freetype/ftimage.h"),
        "FTL.TXT": read_lf(SRC / "FTL.TXT"),
    }
    for name, text in files.items():
        (DST / name).write_bytes(text.encode("utf-8"))
        print(f"wrote {(DST / name).relative_to(ROOT)}")


if __name__ == "__main__":
    main()
