enable_language(C)

# The real FlashLangPartition over an in-memory `spiffs` partition: this directory's esp_partition.h
# and esp_rom_crc.h stand in for ESP-IDF's, so they come first on the include path. uzlib is
# font_group_stream's uzlib_font_ring. Logging stays compiled out.
add_executable(FlashLangPartitionTest
  FlashLangPartitionTest.cpp
  ${REPO_ROOT}/lib/hal/FlashLangPartition.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/test/uzlib_lzcopy/UzlibChksumStubs.cpp
)
target_include_directories(FlashLangPartitionTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
)
target_link_libraries(FlashLangPartitionTest PRIVATE
  crosspoint_test_common
  uzlib_font_ring
  GTest::gtest_main
)
gtest_discover_tests(FlashLangPartitionTest)
