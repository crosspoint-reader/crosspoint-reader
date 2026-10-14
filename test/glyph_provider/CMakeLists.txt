add_executable(GlyphProviderTest
  GlyphProviderTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(GlyphProviderTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(GlyphProviderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphProviderTest)
