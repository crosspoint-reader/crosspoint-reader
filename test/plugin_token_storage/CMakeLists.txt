find_package(Python3 REQUIRED COMPONENTS Interpreter)
FetchContent_Declare(arduinojson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(arduinojson)
set(TOKEN_SOURCE "${REPO_ROOT}/src/util/PluginHttp.cpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${TOKEN_SOURCE}")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/extract.py"
          "${TOKEN_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/TokenFunctions.inc"
  RESULT_VARIABLE TOKEN_EXTRACT_RESULT
)
if(NOT TOKEN_EXTRACT_RESULT EQUAL 0)
  message(FATAL_ERROR "Could not extract token storage functions")
endif()
add_executable(PluginTokenStorageTest PluginTokenStorageTest.cpp)
target_include_directories(PluginTokenStorageTest PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/stubs" "${CMAKE_CURRENT_BINARY_DIR}" "${REPO_ROOT}/src"
  "${arduinojson_SOURCE_DIR}/src"
)
target_link_libraries(PluginTokenStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginTokenStorageTest)
