add_executable(FallbackFaceProviderTest
  FallbackFaceProviderTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/FallbackFaceProvider.cpp
  ${REPO_ROOT}/lib/EpdFont/GlyphFallback.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(FallbackFaceProviderTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FallbackFaceProviderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FallbackFaceProviderTest)
