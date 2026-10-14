add_executable(GlyphStoreTest
  SimNorFlashTest.cpp
  GlyphStoreFormatTest.cpp
  GlyphStoreInstallTest.cpp
  GlyphStoreRecoveryTest.cpp
  GlyphStoreGlyphTest.cpp
  GlyphStoreSizesPairsWipeTest.cpp
  GlyphStorePowerCutTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/GlyphStoreFormat.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/FlashGlyphStore.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(GlyphStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_link_libraries(GlyphStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphStoreTest)
