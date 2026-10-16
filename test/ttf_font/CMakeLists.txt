# TtfFont (lib/EpdFont/ttf): TrueType families on the glyph store, from the file probe to drawn pages.

add_executable(TtfFontTest
  TtfFamilyTest.cpp
  TtfFontTest.cpp
  TtfRegistryTest.cpp
  TtfOpenTypeTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfOpenType.cpp
  # the SDK's OpenType layout readers TtfOpenType uses (the firmware links them through FreeInkOpenType)
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/Gpos.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/Gsub.cpp
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont/src/KernTable.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

# test/shims before lib/hal: the host HalStorage, but the real NorFlash.h.
target_include_directories(TtfFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/ttf_rasterizer
  ${REPO_ROOT}/test/glyph_store
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkOpenType/include
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(TtfFontTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfFontTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# The sanitizers ttf_rasterizer found for this toolchain (ASan + UBSan on CI's Linux GCC).
target_compile_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfFontTest PRIVATE ${TTF_SANITIZERS})

gtest_discover_tests(TtfFontTest DISCOVERY_TIMEOUT 60)
