add_executable(ImageMemoryLadderTest
  ImageMemoryLadderTest.cpp
)

target_include_directories(ImageMemoryLadderTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ImageMemoryLadderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ImageMemoryLadderTest)
