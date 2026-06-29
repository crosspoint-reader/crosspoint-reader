add_executable(DictEngineTest
  DictEngineTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
)

target_include_directories(DictEngineTest PRIVATE
  ${REPO_ROOT}/test/stubs  # overrides HalStorage.h / Logging.h (device-only)
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/src
)

target_compile_definitions(DictEngineTest PRIVATE
  DICT_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../dictionaries"
)

target_link_libraries(DictEngineTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(DictEngineTest generate_test_dictionaries)

gtest_discover_tests(DictEngineTest)
