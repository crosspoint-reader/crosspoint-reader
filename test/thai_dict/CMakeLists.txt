add_executable(ThaiDictTest
  ThaiDictTest.cpp
  ${REPO_ROOT}/lib/ThaiDict/ThaiDict.cpp
)

target_include_directories(ThaiDictTest PRIVATE
  ${REPO_ROOT}/lib/ThaiDict
)

target_link_libraries(ThaiDictTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ThaiDictTest)
