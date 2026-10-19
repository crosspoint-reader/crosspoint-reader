add_executable(ScaledGlyphResampleTest
  ScaledGlyphResampleTest.cpp
)

target_include_directories(ScaledGlyphResampleTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(ScaledGlyphResampleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ScaledGlyphResampleTest)
