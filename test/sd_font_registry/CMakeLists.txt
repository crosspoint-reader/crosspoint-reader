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

add_executable(VectorFontRegistryTest
  VectorFontIdentityTest.cpp
  ${REPO_ROOT}/lib/EpdFont/VectorFontIdentity.cpp
  VectorFontRegistryFileNames.cpp
  VectorFontRegistryStyleSelection.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
)
target_compile_definitions(VectorFontRegistryTest PRIVATE BOARD_HAS_PSRAM)
target_include_directories(VectorFontRegistryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/library_builder/stubs
)
target_link_libraries(VectorFontRegistryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(VectorFontRegistryTest)
