add_executable(ChunkedVectorTest
  ChunkedVectorTest.cpp
)

target_include_directories(ChunkedVectorTest PRIVATE
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(ChunkedVectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChunkedVectorTest)
