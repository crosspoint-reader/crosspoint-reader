find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(MARQUEE_HOST_SOURCE "${CMAKE_CURRENT_BINARY_DIR}/EndOfBookOptionsHost.cpp")
add_custom_command(
  OUTPUT "${MARQUEE_HOST_SOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/generate_host_source.py"
    "${REPO_ROOT}" "${MARQUEE_HOST_SOURCE}"
  DEPENDS
    generate_host_source.py
    "${REPO_ROOT}/src/activities/reader/EndOfBookOptions.cpp"
    "${REPO_ROOT}/src/activities/reader/ReaderActivity.cpp"
  VERBATIM
)

add_executable(EndOfBookOptionsTest EndOfBookOptionsTest.cpp "${MARQUEE_HOST_SOURCE}")
target_include_directories(EndOfBookOptionsTest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs"
  "${REPO_ROOT}/src/activities/reader"
)
target_link_libraries(EndOfBookOptionsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EndOfBookOptionsTest)
