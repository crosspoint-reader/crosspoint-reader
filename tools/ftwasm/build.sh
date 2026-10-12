#!/usr/bin/env bash
# Reproducible FreeType -> WebAssembly build for the browser .cpfont converter.
#
#   ./build.sh                          # default: FreeType 2.13.3 + lean HarfBuzz 8.3.0
#   FT_VERSION=2.13.2 HARFBUZZ=none ./build.sh
#
# Knobs (environment):
#   FT_VERSION     2.13.3 (default) | 2.13.2        -- only versions with a pinned sha256
#   HARFBUZZ       lean (default) | full | none     -- autofit OpenType coverage via HarfBuzz 8.3.0
#   EMSDK_VERSION  6.0.10 (default)                 -- emsdk tag == emscripten version
#   OPT_FLAGS      compiler optimisation flags (default -Oz)
#   OUT            output dir (default dist/ft<ver>-hb<mode>)
#
# Everything (emsdk, tarballs, objects) stays under this script's directory.
# Each tarball is checked against a pinned sha256 before it is unpacked.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FT_VERSION="${FT_VERSION:-2.13.3}"
HARFBUZZ="${HARFBUZZ:-lean}"
EMSDK_VERSION="${EMSDK_VERSION:-6.0.10}"
HB_VERSION="8.3.0"
OUT="${OUT:-$HERE/dist/ft${FT_VERSION}-hb${HARFBUZZ}}"

# sha256 of the release tarballs. 2.13.2/2.13.3: identical on savannah and
# sourceforge, and both carry a good GPG signature by Werner Lemberg
# (E30674707856409FF1948010BE6C3AAC63AD8E3F). 2.13.2 and HarfBuzz 8.3.0 are
# also the exact pins in freetype-py 2.5.1's setup-build-freetype.py.
declare -A FT_SHA256=(
  [2.13.2]=12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d
  [2.13.3]=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
)
HB_SHA256=109501eaeb8bde3eadb25fab4164e993fbace29c3d775bcaa1c1e58e2f15f847

case "$HARFBUZZ" in none|lean|full) ;; *) echo "HARFBUZZ must be none|lean|full" >&2; exit 2;; esac
[[ -n "${FT_SHA256[$FT_VERSION]:-}" ]] || { echo "no pinned sha256 for FreeType $FT_VERSION" >&2; exit 2; }

DL="$HERE/dl"
BUILD="$HERE/build"
JOBS="$(nproc 2>/dev/null || echo 4)"

log() { printf '\n== %s\n' "$*"; }

# fetch <dest> <sha256> <url> [<url>...] -- first URL that yields the right bytes wins.
fetch() {
  local dest="$1" sha="$2"; shift 2
  mkdir -p "$(dirname "$dest")"
  if [[ -f "$dest" ]] && echo "$sha  $dest" | sha256sum -c --quiet - 2>/dev/null; then return 0; fi
  local url
  for url in "$@"; do
    echo "fetching $url"
    if curl -fsSL --retry 3 -m 600 -o "$dest.part" "$url" &&
       echo "$sha  $dest.part" | sha256sum -c --quiet -; then
      mv "$dest.part" "$dest"; return 0
    fi
    echo "  checksum/download failed for $url" >&2
    rm -f "$dest.part"
  done
  echo "could not fetch a verified $(basename "$dest")" >&2; exit 1
}

# --- emsdk ---------------------------------------------------------------
log "emsdk $EMSDK_VERSION"
if [[ ! -x "$HERE/emsdk/emsdk" ]]; then
  git clone -q https://github.com/emscripten-core/emsdk.git "$HERE/emsdk"
fi
if [[ "$(git -C "$HERE/emsdk" describe --tags --exact-match 2>/dev/null || true)" != "$EMSDK_VERSION" ]]; then
  git -C "$HERE/emsdk" fetch -q --tags
  git -C "$HERE/emsdk" checkout -q "$EMSDK_VERSION"
fi
"$HERE/emsdk/emsdk" install "$EMSDK_VERSION" >/dev/null
"$HERE/emsdk/emsdk" activate "$EMSDK_VERSION" >/dev/null
# shellcheck disable=SC1091
source "$HERE/emsdk/emsdk_env.sh" >/dev/null 2>&1
emcc --version | head -1

# --- sources -------------------------------------------------------------
log "FreeType $FT_VERSION"
FT_TAR="$DL/freetype/freetype-$FT_VERSION.tar.xz"
fetch "$FT_TAR" "${FT_SHA256[$FT_VERSION]}" \
  "https://download.savannah.gnu.org/releases/freetype/freetype-$FT_VERSION.tar.xz" \
  "https://downloads.sourceforge.net/project/freetype/freetype2/$FT_VERSION/freetype-$FT_VERSION.tar.xz"
mkdir -p "$BUILD/src"
FT_SRC="$BUILD/src/freetype-$FT_VERSION"
rm -rf "$FT_SRC"; tar -xf "$FT_TAR" -C "$BUILD/src"

if [[ "$HARFBUZZ" != none ]]; then
  log "HarfBuzz $HB_VERSION ($HARFBUZZ)"
  HB_TAR="$DL/harfbuzz/harfbuzz-$HB_VERSION.tar.xz"
  fetch "$HB_TAR" "$HB_SHA256" \
    "https://github.com/harfbuzz/harfbuzz/releases/download/$HB_VERSION/harfbuzz-$HB_VERSION.tar.xz"
  HB_SRC="$BUILD/src/harfbuzz-$HB_VERSION"
  rm -rf "$HB_SRC"; tar -xf "$HB_TAR" -C "$BUILD/src"
fi

# --- configuration -------------------------------------------------------
# ftoption.h = upstream defaults (what freetype-py's CMake build uses), except:
#   * FT_CONFIG_OPTION_USE_HARFBUZZ on when HarfBuzz is linked (freetype-py has it on);
#   * zlib/LZW off: no gzip module, so WOFF/.Z are not accepted (TTF/OTF only).
# Nothing else that touches loading, hinting or rasterising is changed.
VARIANT="ft${FT_VERSION}-hb${HARFBUZZ}${OPT_FLAGS:+-$(echo "$OPT_FLAGS" | tr -d " ")}"
OBJ="$BUILD/obj/$VARIANT"
CFG="$OBJ/config"
rm -rf "$OBJ"; mkdir -p "$CFG"
sed -e 's|^#define FT_CONFIG_OPTION_USE_ZLIB$|/* #define FT_CONFIG_OPTION_USE_ZLIB */|' \
    -e 's|^#define FT_CONFIG_OPTION_USE_LZW$|/* #define FT_CONFIG_OPTION_USE_LZW */|' \
    "$FT_SRC/include/freetype/config/ftoption.h" > "$CFG/ftwasm-option.h"
if [[ "$HARFBUZZ" != none ]]; then
  sed -i -e 's|^/\* #define FT_CONFIG_OPTION_USE_HARFBUZZ \*/$|#define FT_CONFIG_OPTION_USE_HARFBUZZ|' "$CFG/ftwasm-option.h"
  grep -q '^#define FT_CONFIG_OPTION_USE_HARFBUZZ$' "$CFG/ftwasm-option.h" || { echo "HarfBuzz switch not found" >&2; exit 1; }
fi
cp "$HERE/csrc/ftmodule.h" "$CFG/ftwasm-modules.h"

read -r -a OPT <<< "${OPT_FLAGS:--Oz}"   # -Oz: 5 % smaller than -Os, ~7 % slower; -flto gains <1 %
CFLAGS=("${OPT[@]}" -sSUPPORT_LONGJMP=wasm
        -DFT2_BUILD_LIBRARY
        "-DFT_CONFIG_OPTIONS_H=<ftwasm-option.h>"
        "-DFT_CONFIG_MODULES_H=<ftwasm-modules.h>"
        -I"$CFG" -I"$FT_SRC/include")
[[ "$HARFBUZZ" != none ]] && CFLAGS+=(-I"$HB_SRC/src")

FT_UNITS=(
  src/base/ftsystem.c src/base/ftinit.c src/base/ftdebug.c src/base/ftbase.c
  src/base/ftbitmap.c src/base/ftmm.c
  src/autofit/autofit.c src/truetype/truetype.c src/cff/cff.c
  src/psaux/psaux.c src/psnames/psnames.c src/sfnt/sfnt.c src/smooth/smooth.c
)

log "compile ($VARIANT)"
compile_c() { emcc "${CFLAGS[@]}" -c "$1" -o "$2"; }
OBJS=()
pids=()
for u in "${FT_UNITS[@]}"; do
  o="$OBJ/$(basename "${u%.c}").o"; OBJS+=("$o")
  compile_c "$FT_SRC/$u" "$o" & pids+=($!)
done
compile_c "$HERE/csrc/ftwasm.c" "$OBJ/ftwasm.o" & pids+=($!)
OBJS+=("$OBJ/ftwasm.o")

if [[ "$HARFBUZZ" != none ]]; then
  HBFLAGS=("${OPT[@]}" -sSUPPORT_LONGJMP=wasm -fno-exceptions -fno-rtti -fno-threadsafe-statics -w
           -DHB_NO_MT -DHB_NO_FALLBACK_SHAPE)
  # HB_NO_FALLBACK_SHAPE only drops the "no OpenType tables" fallback shaper;
  # FreeType only shapes fonts through the OT shaper.
  if [[ "$HARFBUZZ" == lean ]]; then
    HBFLAGS+=(-DHB_CONFIG_OVERRIDE_H='"hb-ftwasm-config.h"' -I"$HERE/csrc")
  fi
  emcc "${HBFLAGS[@]}" -I"$HB_SRC/src" -c "$HB_SRC/src/harfbuzz.cc" -o "$OBJ/harfbuzz.o" & pids+=($!)
  OBJS+=("$OBJ/harfbuzz.o")
fi
for p in "${pids[@]}"; do wait "$p"; done

# --- link ----------------------------------------------------------------
log "link"
EXPORTS=_malloc,_free,_ftw_init,_ftw_version,_ftw_new_memory_face,_ftw_done_face
EXPORTS+=,_ftw_set_char_size,_ftw_set_pixel_sizes,_ftw_get_char_index,_ftw_load_glyph
EXPORTS+=,_ftw_render_glyph,_ftw_glyph_info,_ftw_size_metrics,_ftw_face_info
mkdir -p "$OUT"
emcc "${OPT[@]}" "${OBJS[@]}" -o "$OUT/ftwasm.mjs" \
  -sSUPPORT_LONGJMP=wasm \
  -sMODULARIZE=1 -sEXPORT_ES6=1 -sEXPORT_NAME=createFtWasm \
  -sENVIRONMENT=web,worker,node \
  -sFILESYSTEM=0 -sDYNAMIC_EXECUTION=0 \
  -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=16MB -sSTACK_SIZE=256KB \
  -sEXPORTED_FUNCTIONS="$EXPORTS" \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAP32,UTF8ToString \
  -sINCOMING_MODULE_JS_API=locateFile,wasmBinary,instantiateWasm,print,printErr \
  --closure 1

# --- record what was built ----------------------------------------------
{
  echo "variant:        $VARIANT"
  echo "freetype:       $FT_VERSION sha256=${FT_SHA256[$FT_VERSION]}"
  [[ "$HARFBUZZ" != none ]] && echo "harfbuzz:       $HB_VERSION sha256=$HB_SHA256 ($HARFBUZZ)"
  echo "emscripten:     $(emcc --version | head -1)"
  echo "wasm bytes:     $(stat -c %s "$OUT/ftwasm.wasm")"
  echo "wasm gzip -9:   $(gzip -9 -n -c "$OUT/ftwasm.wasm" | wc -c)"
  echo "glue bytes:     $(stat -c %s "$OUT/ftwasm.mjs")"
  echo "glue gzip -9:   $(gzip -9 -n -c "$OUT/ftwasm.mjs" | wc -c)"
  echo "wasm sha256:    $(sha256sum "$OUT/ftwasm.wasm" | cut -d' ' -f1)"
} | tee "$OUT/BUILDINFO.txt"
