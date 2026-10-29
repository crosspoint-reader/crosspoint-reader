add_executable(BmpToBmpTest BmpToBmpTest.cpp
  ${REPO_ROOT}/lib/BmpToBmpConverter/BmpToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp)
target_include_directories(BmpToBmpTest PRIVATE
  ${REPO_ROOT}/test/gif_support/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/BmpToBmpConverter
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory)
target_link_libraries(BmpToBmpTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BmpToBmpTest)
