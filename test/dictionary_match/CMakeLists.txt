add_executable(DictionaryMatchTest
  DictionaryMatchTest.cpp
)

target_include_directories(DictionaryMatchTest PRIVATE
  ${REPO_ROOT}/src/util
)

target_link_libraries(DictionaryMatchTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryMatchTest)
