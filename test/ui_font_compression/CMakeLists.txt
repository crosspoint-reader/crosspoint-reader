enable_language(C)

# The real FontDecompressor over the real compressed CJK UI faces. Same build as font_group_stream:
# uzlib from its own static library, and that directory's Arduino.h for the micros() getBitmap()
# times itself with. Logging stays compiled out, so no log stub is needed.
add_executable(UiFontCompressionTest
  UiFontCompressionTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
)
target_include_directories(UiFontCompressionTest PRIVATE
  ${REPO_ROOT}/test/font_group_stream
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(UiFontCompressionTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(UiFontCompressionTest)
