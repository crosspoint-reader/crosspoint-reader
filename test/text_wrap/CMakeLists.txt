add_executable(TextWrapTest
  TextWrapTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TextWrapTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(TextWrapTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextWrapTest)
