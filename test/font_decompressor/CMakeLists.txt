find_package(ZLIB REQUIRED)

add_executable(FontDecompressorTest
  FontDecompressorTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(FontDecompressorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FontDecompressorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ZLIB::ZLIB
)

gtest_discover_tests(FontDecompressorTest)
