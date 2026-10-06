# libwebp (decoder only)

Google's libwebp **v1.4.0** (https://github.com/webmproject/libwebp, tag
v1.4.0), BSD-3-Clause with the WebM patent grant: see `COPYING` and
`PATENTS`. Vendored unmodified for decoding the WebP images in Kiwix ZIM
files. Removed: the encoder, mux and demux, the SIMD/MIPS variants (none
apply to the ESP32-S3), and the build files. Includes are relative to this
folder (`src/webp/decode.h`).
