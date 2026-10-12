#!/usr/bin/env bash
# Native libfreetype.so for the Python side of the parity test, built with the
# exact recipe of freetype-py 2.5.1's setup-build-freetype.py (FreeType without
# HarfBuzz -> HarfBuzz 8.3.0 against it -> FreeType again with HarfBuzz, -O2 -m64,
# no PNG/BZip2/zlib/Brotli), but with a selectable FreeType version. Then makes
# a venv (venv-ft<ver>) whose freetype-py loads that library instead of the
# wheel's bundled 2.13.2.
#
#   FT_VERSION=2.13.3 test/build_native_ft.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FT_VERSION="${FT_VERSION:-2.13.3}"
HB_VERSION=8.3.0
declare -A FT_SHA256=(
  [2.13.2]=12991c4e55c506dd7f9b765933e62fd2be2e06d421505d7950a132e4f1bb484d
  [2.13.3]=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289
)
HB_SHA256=109501eaeb8bde3eadb25fab4164e993fbace29c3d775bcaa1c1e58e2f15f847
FT_TAR="$HERE/dl/freetype/freetype-$FT_VERSION.tar.xz"
HB_TAR="$HERE/dl/harfbuzz/harfbuzz-$HB_VERSION.tar.xz"
echo "${FT_SHA256[$FT_VERSION]}  $FT_TAR" | sha256sum -c --quiet -
echo "$HB_SHA256  $HB_TAR" | sha256sum -c --quiet -

W="$HERE/build/native-ft$FT_VERSION"
rm -rf "$W"; mkdir -p "$W"
tar -xf "$FT_TAR" -C "$W"; tar -xf "$HB_TAR" -C "$W"
PREFIX="$W/local"
G=(-G Ninja -DCMAKE_COLOR_MAKEFILE=false -DCMAKE_PREFIX_PATH="$PREFIX" -DCMAKE_INSTALL_PREFIX="$PREFIX"
   -DCMAKE_C_FLAGS="-m64 -O2" -DCMAKE_CXX_FLAGS="-m64 -O2" -DCMAKE_LD_FLAGS="-m64"
   -DCMAKE_POLICY_VERSION_MINIMUM=3.5)
NOLIBS=(-DCMAKE_DISABLE_FIND_PACKAGE_PNG=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_BZip2=TRUE
        -DCMAKE_DISABLE_FIND_PACKAGE_ZLIB=TRUE -DCMAKE_DISABLE_FIND_PACKAGE_BrotliDec=TRUE)
FTB="$W/freetype-$FT_VERSION/build"; HBB="$W/harfbuzz-$HB_VERSION/build"
mkdir -p "$FTB" "$HBB"

(cd "$FTB" && cmake "${G[@]}" -DBUILD_SHARED_LIBS=OFF -DCMAKE_DISABLE_FIND_PACKAGE_HarfBuzz=TRUE -DFT_WITH_HARFBUZZ=OFF \
   "${NOLIBS[@]}" .. >/dev/null && cmake --build . --config Release --target install --parallel >/dev/null)
(cd "$HBB" && cmake "${G[@]}" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
   -DHB_HAVE_FREETYPE=ON -DHB_HAVE_GLIB=OFF -DHB_HAVE_CORETEXT=OFF .. >/dev/null &&
   cmake --build . --config Release --target install --parallel >/dev/null)
(cd "$FTB" && cmake "${G[@]}" -DBUILD_SHARED_LIBS=ON -DCMAKE_DISABLE_FIND_PACKAGE_HarfBuzz=FALSE -DFT_WITH_HARFBUZZ=ON \
   "${NOLIBS[@]}" -DPKG_CONFIG_EXECUTABLE="" -DHARFBUZZ_INCLUDE_DIRS="$PREFIX/include/harfbuzz" -DSKIP_INSTALL_HEADERS=ON .. >/dev/null &&
   cmake --build . --config Release --target install --parallel >/dev/null)
SO="$(ls "$PREFIX"/lib*/libfreetype.so.6.* | head -1)"
echo "built $SO"

V="$HERE/venv-ft$FT_VERSION"
if [[ ! -x "$V/bin/python" ]]; then
  python3 -m venv "$V"
  "$V/bin/pip" install -q --disable-pip-version-check freetype-py==2.5.1 fonttools==4.63.0
fi
cp "$SO" "$V"/lib/python3*/site-packages/freetype/libfreetype.so
"$V/bin/python" -I -c "import freetype; print('venv', '$V', 'FreeType', freetype.version())"
