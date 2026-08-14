add_executable(TokenBoundaryTest
  TokenBoundaryTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TokenBoundaryTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TokenBoundaryTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TokenBoundaryTest)
