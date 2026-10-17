# SdFat 2.3.1 as the firmware builds it: the release platformio.ini pins, with
# scripts/sdfat_patches/ applied by scripts/patch_sdfat.py (the same digest gates
# as the firmware build), over an in-memory card. Fetched at configure time like
# googletest, since CI's unit-test job runs no PlatformIO build.
include(FetchContent)

FetchContent_Declare(
  sdfat
  URL https://github.com/greiman/SdFat/archive/refs/tags/2.3.1.tar.gz
  URL_HASH SHA256=a86ac8c6551f8294689569cf748f6aa81ceddd2317ef4f0815a0bb39b365c769
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  # SdFat has no CMakeLists.txt; populate only.
  SOURCE_SUBDIR no-cmake
)
FetchContent_MakeAvailable(sdfat)

# Patch a fresh copy, so an edited patch never meets a copy patched by the last one.
set(SDFAT_PATCHED_DIR "${CMAKE_CURRENT_BINARY_DIR}/SdFat")
file(REMOVE_RECURSE "${SDFAT_PATCHED_DIR}")
file(COPY "${sdfat_SOURCE_DIR}/library.properties" "${sdfat_SOURCE_DIR}/src" DESTINATION "${SDFAT_PATCHED_DIR}")
file(GLOB SDFAT_PATCH_FILES "${REPO_ROOT}/scripts/sdfat_patches/*.patch")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${REPO_ROOT}/scripts/patch_sdfat.py" ${SDFAT_PATCH_FILES})
execute_process(
  COMMAND ${Python3_EXECUTABLE} "${REPO_ROOT}/scripts/patch_sdfat.py" "${SDFAT_PATCHED_DIR}"
  RESULT_VARIABLE SDFAT_PATCH_RESULT
)
if(NOT SDFAT_PATCH_RESULT EQUAL 0)
  message(FATAL_ERROR "scripts/patch_sdfat.py failed on ${SDFAT_PATCHED_DIR}")
endif()

file(GLOB SDFAT_HOST_SOURCES
  "${SDFAT_PATCHED_DIR}/src/FatLib/*.cpp"
  "${SDFAT_PATCHED_DIR}/src/ExFatLib/*.cpp"
  "${SDFAT_PATCHED_DIR}/src/FsLib/*.cpp"
  "${SDFAT_PATCHED_DIR}/src/common/*.cpp"
)
add_library(sdfat_host STATIC ${SDFAT_HOST_SOURCES})
# host/ first: it stands in for SdCard/SdCard.h, which only the SPI card driver needs.
target_include_directories(sdfat_host PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/host" "${SDFAT_PATCHED_DIR}/src")
# The SdFat options platformio.ini sets, plus the block-device API the X4 Pro mounts through.
target_compile_definitions(sdfat_host PUBLIC
  ENABLE_ARDUINO_FEATURES=0
  ENABLE_ARDUINO_SERIAL=0
  ENABLE_ARDUINO_STRING=0
  USE_BLOCK_DEVICE_INTERFACE=1
  USE_SEPARATE_FAT_CACHE=1
  MAINTAIN_FREE_CLUSTER_COUNT=1
  USE_UTF8_LONG_NAMES=1
  DESTRUCTOR_CLOSES_FILE=1
)
target_compile_options(sdfat_host PUBLIC -include "${CMAKE_CURRENT_SOURCE_DIR}/host/host_prelude.h")
target_compile_options(sdfat_host PRIVATE -w)  # third-party code, as it ships

add_executable(SdFatAllocTest SdFatAllocTest.cpp)
target_link_libraries(SdFatAllocTest PRIVATE crosspoint_test_common sdfat_host GTest::gtest_main)
gtest_discover_tests(SdFatAllocTest)
