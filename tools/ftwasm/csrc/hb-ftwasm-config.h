/*
 * Lean HarfBuzz 8.3.0 for FreeType's auto-hinter (src/autofit/afshaper.c).
 *
 * The auto-hinter uses HarfBuzz only to (a) collect which glyphs GSUB
 * features reach (style coverage: e.g. superscript/small-cap glyphs get
 * their own blue zones) and (b) shape blue-zone characters with those
 * features.  Both only look at glyph ids, never at positions, drawing or
 * names.  The switches below remove code that cannot change those glyph ids.
 *
 * Deliberately kept: GSUB collect-glyphs/closure, feature variations
 * (HB_NO_VAR would change which lookups a variable font's default
 * instance applies), the complex shapers and their no-GSUB fallbacks,
 * and setlocale() (default shaping language, as in the Python process).
 *
 * One known divergence: HB_NO_AAT drops Apple 'morx' shaping, so a font
 * that has morx but no GSUB could hint differently than the native build.
 */
#define HB_DISABLE_DEPRECATED
#define HB_NDEBUG
#define HB_OPTIMIZE_SIZE
#define HB_NO_AAT
#define HB_NO_ATEXIT
#define HB_NO_BITMAP
#define HB_NO_BUFFER_MESSAGE
#define HB_NO_BUFFER_SERIALIZE
#define HB_NO_BUFFER_VERIFY
#define HB_NO_CFF
#define HB_NO_COLOR
#define HB_NO_DRAW
#define HB_NO_ERRNO
#define HB_NO_FACE_COLLECT_UNICODES
#define HB_NO_GETENV
#define HB_NO_HINTING
#define HB_NO_MATH
#define HB_NO_META
#define HB_NO_METRICS
#define HB_NO_MMAP
#define HB_NO_NAME
#define HB_NO_OPEN
#define HB_NO_OT_FONT_GLYPH_NAMES
#define HB_NO_PAINT
#define HB_NO_STYLE
#define HB_NO_SUBSET_LAYOUT
