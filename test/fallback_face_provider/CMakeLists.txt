enable_language(C)

# The real FontDecompressor too: the CJK UI faces are compressed, and the provider decodes their
# glyphs through it. uzlib comes from font_group_stream's uzlib_font_ring, and that directory's
# Arduino.h supplies the micros() getBitmap() times itself with.
add_executable(FallbackFaceProviderTest
  FallbackFaceProviderTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FallbackFaceProvider.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
)

target_include_directories(FallbackFaceProviderTest PRIVATE
  ${REPO_ROOT}/test/font_group_stream
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FallbackFaceProviderTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)

gtest_discover_tests(FallbackFaceProviderTest)
