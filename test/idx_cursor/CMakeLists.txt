add_executable(IdxCursorTest
  IdxCursorTest.cpp
)

target_include_directories(IdxCursorTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(IdxCursorTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IdxCursorTest)
