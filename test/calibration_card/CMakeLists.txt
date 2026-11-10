add_executable(CalibrationCardTest CalibrationCardTest.cpp ${REPO_ROOT}/src/util/CalibrationCard.cpp)
target_include_directories(CalibrationCardTest PRIVATE ${REPO_ROOT}/src/util)
target_link_libraries(CalibrationCardTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CalibrationCardTest)
