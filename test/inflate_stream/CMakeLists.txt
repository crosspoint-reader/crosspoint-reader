add_executable(InflateStreamTest
  InflateStreamTest.cpp
  ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/hal/HalMemory.cpp
)
target_include_directories(InflateStreamTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/miniz/src
)
target_link_libraries(InflateStreamTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(InflateStreamTest)
