add_executable(ReaderFontSizesTest
  ReaderFontSizesTest.cpp
  ${REPO_ROOT}/src/ReaderFontSizes.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
)
target_include_directories(ReaderFontSizesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(ReaderFontSizesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReaderFontSizesTest)
