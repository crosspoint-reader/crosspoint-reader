add_executable(SdCardFontTest
  SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
  ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp
  ${OT_SHAPER_SOURCES}
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SdCardFontTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${OT_SHAPER_INCLUDE_DIR}
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(SdCardFontTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_compile_options(SdCardFontTest PRIVATE -fno-exceptions)
gtest_discover_tests(SdCardFontTest)

# Exercise the same font fixtures with the optional PSRAM read staging enabled.
add_executable(SdCardFontPsramTest
  SdCardFontTest.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
  ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp
  ${OT_SHAPER_SOURCES}
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(SdCardFontPsramTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
  ${OT_SHAPER_INCLUDE_DIR}
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
target_compile_definitions(SdCardFontPsramTest PRIVATE BOARD_HAS_PSRAM)
target_compile_options(SdCardFontPsramTest PRIVATE -fno-exceptions)
target_link_libraries(SdCardFontPsramTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SdCardFontPsramTest TEST_PREFIX "Psram.")
