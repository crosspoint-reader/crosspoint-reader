# Built-in font generation

Install `requirements.txt` in a Python environment, then run
`bash convert-builtin-fonts.sh` with that environment active. Source fonts are
under `../builtinFonts/source/`. Generated built-in headers are checked in.

Reader fonts use row-aligned 2-bit DEFLATE groups. Ubuntu and Noto Sans 8 retain
their packed 1-bit pixels inside DEFLATE groups capped at 2 KiB. The firmware
uses its existing hot-group buffer for these UI fonts; they do not occupy the
four reader prewarm slots. Allocation failure skips the glyph and logs an error.

The conversion script finishes by running `dedupe_font_tables.py` and
`verify_compression.py`. Deduplication moves identical lookup arrays into
`SharedFontTables.h`; glyph metrics and size-dependent kerning values remain
unchanged. Shared names derive from table contents. Individual font headers
include the shared header, so they can still be included independently.

After converting a single font, rerun both scripts against `../builtinFonts/`.
Deduplication supports partial regeneration and repeated runs. Do not edit the
generated shared header directly.

Host checks are `python test/test_dedupe_font_tables.py` from the repository
root and the CMake `FontDecompressorTest` target. The latter compares every UI
glyph against hashes of the original bitmap bytes and checks allocation failure,
reader prewarming, and the UI group allocation bound.
