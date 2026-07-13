add_executable(BookPageMapTest
  BookPageMapTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookPageMap.cpp
)

target_include_directories(BookPageMapTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(BookPageMapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookPageMapTest)
