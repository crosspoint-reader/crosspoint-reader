add_executable(ExternalServicesTest
  ExternalServicesTest.cpp
  ${REPO_ROOT}/src/network/ExternalServices.cpp
)

target_include_directories(ExternalServicesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
  ${REPO_ROOT}/src
)

target_link_libraries(ExternalServicesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ExternalServicesTest)
