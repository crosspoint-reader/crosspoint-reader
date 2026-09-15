add_executable(IMUTiltEstimatorTest
  IMUTiltEstimatorTest.cpp
  ${REPO_ROOT}/lib/hal/HalTiltSensor_IMUTiltEstimator.cpp
)

target_include_directories(IMUTiltEstimatorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
)

target_compile_definitions(IMUTiltEstimatorTest PRIVATE ENABLE_SERIAL_LOG LOG_LEVEL=2)
target_link_libraries(IMUTiltEstimatorTest PRIVATE crosspoint_test_common GTest::gtest_main)

# Compile the same estimator with logging stripped, as in slim firmware builds.
add_library(IMUTiltEstimatorNoLoggingCompileCheck OBJECT
  ${REPO_ROOT}/lib/hal/HalTiltSensor_IMUTiltEstimator.cpp
)
target_include_directories(IMUTiltEstimatorNoLoggingCompileCheck PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs)
target_link_libraries(IMUTiltEstimatorNoLoggingCompileCheck PRIVATE crosspoint_test_common)
add_dependencies(IMUTiltEstimatorTest IMUTiltEstimatorNoLoggingCompileCheck)

gtest_discover_tests(IMUTiltEstimatorTest)

add_executable(HalTiltSensorTest
  HalTiltSensorTest.cpp
  ${REPO_ROOT}/lib/hal/HalTiltSensor.cpp
  ${REPO_ROOT}/lib/hal/HalTiltSensor_IMUTiltEstimator.cpp
)
target_include_directories(HalTiltSensorTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(HalTiltSensorTest PRIVATE ENABLE_SERIAL_LOG LOG_LEVEL=2)
target_link_libraries(HalTiltSensorTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalTiltSensorTest)
