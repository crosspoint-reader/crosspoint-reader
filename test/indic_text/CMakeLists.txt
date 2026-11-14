add_executable(IndicTextTest
  IndicTextTest.cpp
  ${REPO_ROOT}/lib/Utf8/IndicReorder.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(IndicTextTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(IndicTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(IndicTextTest)
