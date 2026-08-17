add_executable(GlyphStreamCodecTest
  GlyphStreamCodecTest.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphStreamCodec.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
)

target_include_directories(GlyphStreamCodecTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(GlyphStreamCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphStreamCodecTest)
