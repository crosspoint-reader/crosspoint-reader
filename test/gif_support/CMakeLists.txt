add_executable(GifSupportTest GifSupportTest.cpp)
target_include_directories(GifSupportTest PRIVATE
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs)
target_link_libraries(GifSupportTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(GifSupportTest)
