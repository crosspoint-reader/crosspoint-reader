add_executable(SdFontRegistryTest
  SdFontRegistryTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
)
# The library builder's in-memory HalStorage already enumerates directories,
# which is all the registry scan needs. Its directory is listed last so the
# real Memory.h (the registry needs shrinkToFit) wins over the stub there.
target_include_directories(SdFontRegistryTest PRIVATE
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/library_builder/stubs
)
target_link_libraries(SdFontRegistryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SdFontRegistryTest)
