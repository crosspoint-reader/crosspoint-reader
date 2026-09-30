# The page turner's own tests. They run alone, on a copy of lib/BlePageTurner and nothing else:
#
#   cmake -S lib/BlePageTurner/test -B build/bt && cmake --build build/bt && ctest --test-dir build/bt
#
# or inside a host's suite with add_subdirectory. They need no radio, no SDK and no host:
# the radio is test/FakeRadio.cpp (the same RadioPort.h the SDK wrapper implements) and the host
# is a set of fake functions. Offline builds point FetchContent at local copies:
#   -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=<googletest>  -DFETCHCONTENT_SOURCE_DIR_ARDUINOJSON=<ArduinoJson>
cmake_minimum_required(VERSION 3.16)
project(ble_page_turner_tests CXX)

if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
  enable_testing()
endif()

include(FetchContent)
if(NOT TARGET GTest::gtest_main)
  FetchContent_Declare(googletest GIT_REPOSITORY https://github.com/google/googletest.git GIT_TAG v1.17.0)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

get_filename_component(BLETURNER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

add_library(bleturner_test_flags INTERFACE)
target_include_directories(bleturner_test_flags INTERFACE ${BLETURNER_ROOT}/include ${BLETURNER_ROOT}/src)
target_compile_options(bleturner_test_flags INTERFACE -Wall -Wextra -Werror)

# ArduinoJson at the version the firmware pins (platformio.ini: bblanchon/ArduinoJson @ 7.4.2). A
# local copy works too: only its src/ is used, never its own build.
FetchContent_Declare(arduinojson GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git GIT_TAG v7.4.2
                     SOURCE_SUBDIR no-build)
FetchContent_MakeAvailable(arduinojson)
add_library(bleturner_arduinojson INTERFACE)
target_include_directories(bleturner_arduinojson SYSTEM INTERFACE ${arduinojson_SOURCE_DIR}/src)

# The rules, pure.
add_executable(BlePageTurnerPolicyTest RadioVerdictTest.cpp HeapRestartTest.cpp KeyBindingTest.cpp)
target_link_libraries(BlePageTurnerPolicyTest PRIVATE bleturner_test_flags GTest::gtest_main)
gtest_discover_tests(BlePageTurnerPolicyTest)

# The runtime against a fake radio and a fake host.
add_executable(BlePageTurnerRuntimeTest RuntimeTest.cpp TickTest.cpp LinkNoteTest.cpp Fakes.cpp ${BLETURNER_ROOT}/src/Runtime.cpp)
target_compile_definitions(BlePageTurnerRuntimeTest PRIVATE BLETURNER_TESTING)
target_link_libraries(BlePageTurnerRuntimeTest PRIVATE bleturner_test_flags GTest::gtest_main)
gtest_discover_tests(BlePageTurnerRuntimeTest)

# A build without BLE.
add_executable(BlePageTurnerNoRadioTest NoRadioTest.cpp ${BLETURNER_ROOT}/src/Runtime.cpp ${BLETURNER_ROOT}/src/NoRadio.cpp)
target_compile_definitions(BlePageTurnerNoRadioTest PRIVATE BLETURNER_TESTING)
target_link_libraries(BlePageTurnerNoRadioTest PRIVATE bleturner_test_flags GTest::gtest_main)
gtest_discover_tests(BlePageTurnerNoRadioTest)

# The settings keys.
add_executable(BlePageTurnerConfigTest ConfigJsonTest.cpp)
target_link_libraries(BlePageTurnerConfigTest PRIVATE bleturner_test_flags bleturner_arduinojson GTest::gtest_main)
gtest_discover_tests(BlePageTurnerConfigTest)

# What the module may include.
add_executable(BlePageTurnerSourceContractTest SourceContractTest.cpp)
target_compile_definitions(BlePageTurnerSourceContractTest PRIVATE BLETURNER_ROOT_PATH="${BLETURNER_ROOT}")
target_link_libraries(BlePageTurnerSourceContractTest PRIVATE bleturner_test_flags GTest::gtest_main)
gtest_discover_tests(BlePageTurnerSourceContractTest)
