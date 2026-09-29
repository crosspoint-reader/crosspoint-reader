add_executable(OtShaperTest
  OtShaperTest.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(OtShaperTest PRIVATE ${OT_SHAPER_INCLUDE_DIR})
target_compile_definitions(OtShaperTest PRIVATE SHAPING_FIXTURE_DIR="${REPO_ROOT}/test/complex_shaper/data")
target_link_libraries(OtShaperTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(OtShaperTest)

# Not a test: the command-line shaper compare_harfbuzz.py diffs against HarfBuzz.
add_executable(ot_shape_cli
  OtShapeCli.cpp
  ${OT_SHAPER_SOURCES}
)
target_include_directories(ot_shape_cli PRIVATE ${OT_SHAPER_INCLUDE_DIR} ${REPO_ROOT}/lib/Utf8)
