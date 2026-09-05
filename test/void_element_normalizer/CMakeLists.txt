add_executable(VoidElementNormalizerTest
  VoidElementNormalizerTest.cpp
)

target_include_directories(VoidElementNormalizerTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub/parsers
)

target_link_libraries(VoidElementNormalizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VoidElementNormalizerTest)
