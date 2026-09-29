add_executable(SdCardFontTest
  SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
  ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp
  ${REPO_ROOT}/lib/HarfBuzz/src/harfbuzz-crosspoint.cc
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SdCardFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${HARFBUZZ_INCLUDE_DIRS}
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
# Same HarfBuzz profile as lib/HarfBuzz/library.json.
target_compile_definitions(SdCardFontTest PRIVATE HB_TINY HB_CONFIG_OVERRIDE_H="hb-crosspoint-config.h")
set_source_files_properties(${REPO_ROOT}/lib/HarfBuzz/src/harfbuzz-crosspoint.cc PROPERTIES COMPILE_OPTIONS "-w")
target_link_libraries(SdCardFontTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(SdCardFontTest PRIVATE -fno-exceptions)
gtest_discover_tests(SdCardFontTest)
