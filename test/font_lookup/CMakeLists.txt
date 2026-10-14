add_executable(FontLookupTest
  FontLookupTest.cpp
)

target_include_directories(FontLookupTest PRIVATE
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(FontLookupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontLookupTest)
