add_executable(FsHelpersTest
  FsHelpersTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(FsHelpersTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shims  # host stand-in for Arduino <WString.h>
  ${REPO_ROOT}/lib/FsHelpers
)

target_link_libraries(FsHelpersTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FsHelpersTest)
