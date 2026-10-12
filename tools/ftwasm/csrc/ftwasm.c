/*
 * ftwasm.c -- the thin C surface the browser .cpfont converter calls.
 *
 * Exposes exactly the FreeType calls fontconvert_sdcard.py makes through
 * freetype-py (FT_New_Memory_Face, FT_Set_Char_Size, FT_Get_Char_Index,
 * FT_Load_Glyph with FT_LOAD_RENDER | FT_LOAD_FORCE_AUTOHINT, and the glyph
 * slot / size metrics it reads afterwards), plus FT_Set_Pixel_Sizes and
 * FT_Render_Glyph for callers that want them.  Struct fields are copied into
 * caller-provided int32 arrays so the JS side never depends on FreeType's
 * struct layout.
 *
 * All FT_Pos / FT_Fixed values are FT_Long, i.e. 32-bit on wasm32.
 */
#include <ft2build.h>
#include FT_FREETYPE_H

#include <emscripten/em_macros.h>
#include <stdint.h>

static FT_Library g_lib;

EMSCRIPTEN_KEEPALIVE int ftw_init(void) {
  if (g_lib) return 0;
  return FT_Init_FreeType(&g_lib);
}

/* out[0..2] = major, minor, patch; out[3] = 1 when built with HarfBuzz. */
EMSCRIPTEN_KEEPALIVE void ftw_version(int32_t* out) {
  FT_Int major = 0, minor = 0, patch = 0;
  if (g_lib) FT_Library_Version(g_lib, &major, &minor, &patch);
  out[0] = major;
  out[1] = minor;
  out[2] = patch;
#ifdef FT_CONFIG_OPTION_USE_HARFBUZZ
  out[3] = 1;
#else
  out[3] = 0;
#endif
}

/* The font bytes must stay alive (not freed) until ftw_done_face(). */
EMSCRIPTEN_KEEPALIVE int ftw_new_memory_face(const FT_Byte* data, int32_t size, int32_t face_index, FT_Face* out_face) {
  *out_face = NULL;
  return FT_New_Memory_Face(g_lib, data, (FT_Long)size, (FT_Long)face_index, out_face);
}

EMSCRIPTEN_KEEPALIVE int ftw_done_face(FT_Face face) { return FT_Done_Face(face); }

EMSCRIPTEN_KEEPALIVE int ftw_set_char_size(FT_Face face, int32_t char_width, int32_t char_height, uint32_t horz_res,
                                           uint32_t vert_res) {
  return FT_Set_Char_Size(face, char_width, char_height, horz_res, vert_res);
}

EMSCRIPTEN_KEEPALIVE int ftw_set_pixel_sizes(FT_Face face, uint32_t width, uint32_t height) {
  return FT_Set_Pixel_Sizes(face, width, height);
}

EMSCRIPTEN_KEEPALIVE uint32_t ftw_get_char_index(FT_Face face, uint32_t charcode) {
  return FT_Get_Char_Index(face, charcode);
}

EMSCRIPTEN_KEEPALIVE int ftw_load_glyph(FT_Face face, uint32_t glyph_index, int32_t load_flags) {
  return FT_Load_Glyph(face, glyph_index, load_flags);
}

EMSCRIPTEN_KEEPALIVE int ftw_render_glyph(FT_Face face, int32_t render_mode) {
  return FT_Render_Glyph(face->glyph, (FT_Render_Mode)render_mode);
}

/* Glyph slot after a load (21 int32s). */
EMSCRIPTEN_KEEPALIVE void ftw_glyph_info(FT_Face face, int32_t* out) {
  FT_GlyphSlot g = face->glyph;
  out[0] = (int32_t)g->bitmap.width;
  out[1] = (int32_t)g->bitmap.rows;
  out[2] = (int32_t)g->bitmap.pitch;
  out[3] = (int32_t)g->bitmap_left;
  out[4] = (int32_t)g->bitmap_top;
  out[5] = (int32_t)(uintptr_t)g->bitmap.buffer;
  out[6] = (int32_t)g->bitmap.pixel_mode;
  out[7] = (int32_t)g->bitmap.num_grays;
  out[8] = (int32_t)g->linearHoriAdvance;
  out[9] = (int32_t)g->linearVertAdvance;
  out[10] = (int32_t)g->advance.x;
  out[11] = (int32_t)g->advance.y;
  out[12] = (int32_t)g->lsb_delta;
  out[13] = (int32_t)g->rsb_delta;
  out[14] = (int32_t)g->metrics.width;
  out[15] = (int32_t)g->metrics.height;
  out[16] = (int32_t)g->metrics.horiBearingX;
  out[17] = (int32_t)g->metrics.horiBearingY;
  out[18] = (int32_t)g->metrics.horiAdvance;
  out[19] = (int32_t)g->glyph_index;
  out[20] = (int32_t)g->format;
}

/* Active size metrics, i.e. face->size->metrics (8 int32s). */
EMSCRIPTEN_KEEPALIVE void ftw_size_metrics(FT_Face face, int32_t* out) {
  FT_Size_Metrics* m = &face->size->metrics;
  out[0] = m->x_ppem;
  out[1] = m->y_ppem;
  out[2] = (int32_t)m->x_scale;
  out[3] = (int32_t)m->y_scale;
  out[4] = (int32_t)m->ascender;
  out[5] = (int32_t)m->descender;
  out[6] = (int32_t)m->height;
  out[7] = (int32_t)m->max_advance;
}

/* Face-level fields (17 int32s); [8]/[9] are char* to NUL-terminated names. */
EMSCRIPTEN_KEEPALIVE void ftw_face_info(FT_Face face, int32_t* out) {
  out[0] = face->units_per_EM;
  out[1] = face->ascender;
  out[2] = face->descender;
  out[3] = face->height;
  out[4] = (int32_t)face->num_glyphs;
  out[5] = (int32_t)face->face_flags;
  out[6] = (int32_t)face->style_flags;
  out[7] = (int32_t)face->num_faces;
  out[8] = (int32_t)(uintptr_t)face->family_name;
  out[9] = (int32_t)(uintptr_t)face->style_name;
  out[10] = face->max_advance_width;
  out[11] = face->underline_position;
  out[12] = face->underline_thickness;
  out[13] = (int32_t)face->bbox.xMin;
  out[14] = (int32_t)face->bbox.yMin;
  out[15] = (int32_t)face->bbox.xMax;
  out[16] = (int32_t)face->bbox.yMax;
}
