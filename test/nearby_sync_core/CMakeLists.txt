add_executable(nearby_sync_core_test
  NearbySyncProtocolTest.cpp
  NearbySyncExchangeTest.cpp
  ${REPO_ROOT}/src/network/NearbySyncExchange.cpp
  ${REPO_ROOT}/src/network/NearbySyncProtocol.cpp
  ${REPO_ROOT}/src/network/NearbySyncRadio.cpp
)

target_link_libraries(nearby_sync_core_test PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(nearby_sync_core_test PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)

target_compile_definitions(nearby_sync_core_test PRIVATE SIMULATOR NEARBY_SYNC_TESTING)

gtest_discover_tests(nearby_sync_core_test)
