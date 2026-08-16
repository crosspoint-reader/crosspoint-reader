add_executable(UiGlyphPoolTest
  UiGlyphPoolTest.cpp
  ${REPO_ROOT}/lib/EpdFont/UiGlyphCodec.cpp
  ${REPO_ROOT}/lib/EpdFont/UiGlyphPool.cpp
)

target_include_directories(UiGlyphPoolTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # no-op Logging.h (real one needs Arduino)
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(UiGlyphPoolTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UiGlyphPoolTest)
