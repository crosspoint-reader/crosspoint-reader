find_package(ZLIB REQUIRED)

add_executable(DictEngineTest
  DictEngineTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/src/util/DictZip.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/tinfgzip.c
  uzlib_zlib_checksums.c
)

target_include_directories(DictEngineTest PRIVATE
  ${REPO_ROOT}/test/stubs  # overrides HalStorage.h / Logging.h (device-only)
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory  # Dictionary.cpp uses makeUniqueNoThrow (Memory.h, flat include like PlatformIO's per-lib root)
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/src
)

target_compile_definitions(DictEngineTest PRIVATE
  DICT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../dictionaries"
)

target_link_libraries(DictEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ZLIB::ZLIB
)

add_dependencies(DictEngineTest generate_test_dictionaries)

gtest_discover_tests(DictEngineTest)
