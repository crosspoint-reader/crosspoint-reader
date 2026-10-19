# src/network/UploadBuffer.h: the 4 KB an HTTP upload gathers its chunks in, taken with nothrow new.
add_executable(UploadBufferTest
  UploadBufferTest.cpp
)

target_include_directories(UploadBufferTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/src/network
)

target_link_libraries(UploadBufferTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UploadBufferTest)
