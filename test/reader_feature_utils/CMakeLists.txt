add_executable(ReaderFeatureUtilsTest
  ReaderFeatureUtilsTest.cpp
  ${REPO_ROOT}/src/util/BookSearchUtils.cpp
  ${REPO_ROOT}/src/util/DictionaryQuery.cpp
  ${REPO_ROOT}/src/util/StringUtils.cpp
  ${REPO_ROOT}/src/util/VietnameseTelex.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ReaderFeatureUtilsTest PRIVATE
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(ReaderFeatureUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderFeatureUtilsTest)
