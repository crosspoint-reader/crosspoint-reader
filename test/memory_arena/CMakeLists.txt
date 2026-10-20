add_executable(BuildArenaTest
  BuildArenaTest.cpp
  PassTableTest.cpp
)

target_include_directories(BuildArenaTest PRIVATE
  ${REPO_ROOT}/lib/Memory
  # The settable esp_heap_caps.h host stub PassTable sizes its heap tables from.
  ${REPO_ROOT}/test/epub_pipeline
)

target_link_libraries(BuildArenaTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BuildArenaTest)
