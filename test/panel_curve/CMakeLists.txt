add_executable(PanelCurveTest PanelCurveTest.cpp ${REPO_ROOT}/src/util/PanelCurve.cpp)
target_include_directories(PanelCurveTest BEFORE PRIVATE stubs ${REPO_ROOT}/src/util)
target_link_libraries(PanelCurveTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PanelCurveTest)
