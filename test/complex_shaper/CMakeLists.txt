add_executable(ComplexShaperTest
  ComplexShaperTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
  ${REPO_ROOT}/lib/HarfBuzz/src/harfbuzz-crosspoint.cc
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ComplexShaperTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${HARFBUZZ_INCLUDE_DIRS}
  ${REPO_ROOT}/lib/Utf8
)

# Same HarfBuzz profile as lib/HarfBuzz/library.json.
target_compile_definitions(ComplexShaperTest PRIVATE
  HB_TINY
  HB_CONFIG_OVERRIDE_H="hb-crosspoint-config.h"
  SHAPING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)
# HarfBuzz is third-party code; keep the project's warning flags off it.
set_source_files_properties(${REPO_ROOT}/lib/HarfBuzz/src/harfbuzz-crosspoint.cc PROPERTIES COMPILE_OPTIONS "-w")

target_link_libraries(ComplexShaperTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ComplexShaperTest)
