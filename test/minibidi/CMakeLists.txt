add_executable(MiniBidiTest
  MiniBidiTest.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
)

target_include_directories(MiniBidiTest PRIVATE
  ${REPO_ROOT}/lib/MiniBidi
)

target_link_libraries(MiniBidiTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(MiniBidiTest)