add_executable(NaturalSortTest
  NaturalSortTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
)

target_link_libraries(NaturalSortTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(NaturalSortTest)
