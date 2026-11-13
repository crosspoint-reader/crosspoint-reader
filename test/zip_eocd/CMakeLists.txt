add_executable(ZipEocdTest
  ZipEocdTest.cpp
)

target_include_directories(ZipEocdTest PRIVATE
  ${REPO_ROOT}/lib/ZipFile
)

target_link_libraries(ZipEocdTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ZipEocdTest)
