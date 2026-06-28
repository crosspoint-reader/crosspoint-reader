add_executable(simulator_heap_test
  SimulatorHeapTest.cpp
  SimulatorHeapTestShim.cpp
  ${REPO_ROOT}/src/simulator/SimulatorHeap.cpp
)

target_compile_definitions(simulator_heap_test PRIVATE
  SIMULATOR
  SIMULATOR_HEAP_TESTING
  SIMULATOR_HEAP_TESTING_NO_GLOBAL_OVERRIDES
)

target_link_libraries(simulator_heap_test PRIVATE
  GTest::gtest_main
  crosspoint_test_common
  espressif_tlsf
)

gtest_discover_tests(simulator_heap_test)
