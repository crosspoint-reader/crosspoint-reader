add_executable(ToneLutTest
  ToneLutTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/ToneLut.cpp
)

target_link_libraries(ToneLutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ToneLutTest)
