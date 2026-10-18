add_executable(ButtonPressNavigationTest ButtonPressNavigationTest.cpp ../../src/util/ButtonNavigator.cpp)
target_include_directories(ButtonPressNavigationTest PRIVATE stubs ../../src)
target_link_libraries(ButtonPressNavigationTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ButtonPressNavigationTest)
