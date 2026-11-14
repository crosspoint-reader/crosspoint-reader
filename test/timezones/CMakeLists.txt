add_executable(TimezonesTest
  TimezonesTest.cpp
  ${REPO_ROOT}/src/util/Timezones.cpp
)

target_include_directories(TimezonesTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/util
)

target_link_libraries(TimezonesTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TimezonesTest)
