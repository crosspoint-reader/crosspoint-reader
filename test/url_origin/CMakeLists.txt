add_executable(UrlOriginTest
  UrlOriginTest.cpp
)

target_include_directories(UrlOriginTest PRIVATE
  ${REPO_ROOT}/lib/UrlOrigin
)

target_link_libraries(UrlOriginTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UrlOriginTest)
