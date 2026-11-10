add_executable(ButtonNavigationTest ButtonNavigationTest.cpp ../../src/util/ButtonNavigator.cpp)
target_include_directories(ButtonNavigationTest PRIVATE stubs ../../src)
target_link_libraries(ButtonNavigationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ButtonNavigationTest)
