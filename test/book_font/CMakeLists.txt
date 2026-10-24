add_executable(BookFontTest BookFontTest.cpp)
target_include_directories(BookFontTest PRIVATE ${REPO_ROOT}/src)
target_link_libraries(BookFontTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookFontTest)
