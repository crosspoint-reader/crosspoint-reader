add_executable(ChunkedVectorTest
  ChunkedVectorTest.cpp
)

target_include_directories(ChunkedVectorTest PRIVATE
  ${REPO_ROOT}/lib/Memory
)

# Lets the tests refuse chunk allocations (see ChunkedVector.h); firmware never defines it.
target_compile_definitions(ChunkedVectorTest PRIVATE CHUNKED_VECTOR_FAULT_INJECTION)

target_link_libraries(ChunkedVectorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChunkedVectorTest)
