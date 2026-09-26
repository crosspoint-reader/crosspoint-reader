add_executable(Bq27220CapacityTest
  Bq27220CapacityTest.cpp
  ${REPO_ROOT}/lib/hal/Bq27220Capacity.cpp
)

target_include_directories(Bq27220CapacityTest PRIVATE
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(Bq27220CapacityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Bq27220CapacityTest)
