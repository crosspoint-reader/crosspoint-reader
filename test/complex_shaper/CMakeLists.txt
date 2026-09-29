add_executable(ComplexShaperTest
  ComplexShaperTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${OT_SHAPER_SOURCES}
)

target_include_directories(ComplexShaperTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${OT_SHAPER_INCLUDE_DIR}
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(ComplexShaperTest PRIVATE
  SHAPING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

target_link_libraries(ComplexShaperTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ComplexShaperTest)
