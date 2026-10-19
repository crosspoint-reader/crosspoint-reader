# lib/hal/HalStorage.cpp as the firmware builds it (-fno-exceptions, asserts on), over the patched
# SdFat that sdfat_alloc builds (sdfat_host) on an in-memory card. stubs/ stands in for the SDK and
# FreeRTOS headers it includes.
add_executable(HalStorageTest
  HalStorageTest.cpp
  ${REPO_ROOT}/lib/hal/HalStorage.cpp
)
set_source_files_properties(${REPO_ROOT}/lib/hal/HalStorage.cpp PROPERTIES COMPILE_OPTIONS "-fno-exceptions")

target_include_directories(HalStorageTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_include_directories(HalStorageTest PRIVATE
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims
)

# A C3 build, without USB Drive: the configuration of the X3 and X4. The asserts stay on whatever
# the build type, since what a test checks is that they hold.
target_compile_definitions(HalStorageTest PRIVATE FREEINK_MCU_C3=1 FREEINK_CAP_USB_MSC=0)
target_compile_options(HalStorageTest PRIVATE -UNDEBUG)

target_link_libraries(HalStorageTest PRIVATE
  crosspoint_test_common
  sdfat_host
  GTest::gtest_main
)

gtest_discover_tests(HalStorageTest)
