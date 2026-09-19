add_executable(BlockStyleTest
  BlockStyleTest.cpp
)

target_include_directories(BlockStyleTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(BlockStyleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BlockStyleTest)
