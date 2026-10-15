# FontCacheManager (lib/GfxRenderer): which font a page's text is prewarmed with. The real SdCardFont and
# FontDecompressor are linked because the manager's other paths use them; the tests here never reach them.
add_executable(FontCacheManagerTest
  FontCacheManagerTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)
target_include_directories(FontCacheManagerTest PRIVATE
  ${REPO_ROOT}/test/font_group_stream  # its Arduino.h adds micros() to the shared shim
  ${REPO_ROOT}/test/zip_entry_reader   # stdio-backed HalStorage.h; shadows test/shims stub
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/test/epub_pipeline  # last: only for its esp_heap_caps.h stub (HeapFit.h)
)
target_link_libraries(FontCacheManagerTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(FontCacheManagerTest)
