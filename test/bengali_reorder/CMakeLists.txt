add_executable(BengaliReorderTest
  BengaliReorderTest.cpp
  ${REPO_ROOT}/lib/Utf8/BengaliReorder.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(BengaliReorderTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(BengaliReorderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BengaliReorderTest)
