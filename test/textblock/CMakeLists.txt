# Both binaries compile the same production sources against the same include
# set. Sharing the lists keeps that guaranteed identical as the sources evolve:
# the allocation counts are only meaningful if the TextBlock.cpp they measure is
# the same translation unit the behavioural tests exercise.
set(TEXTBLOCK_TEST_SOURCES
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

set(TEXTBLOCK_TEST_INCLUDES
  # Stubs must come first: they shadow the Arduino/SD-dependent headers
  # (Logging.h, HalStorage.h, GfxRenderer.h) that TextBlock includes but whose
  # real implementations cannot build on the host.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  # Real EpdFontFamily.h, deliberately not stubbed: its only dependency chain is
  # EpdFont.h -> EpdFontData.h -> <cstdint>, and TextBlock uses just the Style
  # bitmask and the constexpr hasTextDecoration(), so nothing needs linking.
  # Stubbing it would fork the Style values, which are page-cache wire format.
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

add_executable(TextBlockRenderTest
  TextBlockRenderTest.cpp
  ${TEXTBLOCK_TEST_SOURCES}
)

target_include_directories(TextBlockRenderTest PRIVATE ${TEXTBLOCK_TEST_INCLUDES})

target_link_libraries(TextBlockRenderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextBlockRenderTest)

# Separate binary: this one replaces global operator new/delete to count
# allocations, which must not leak into the behavioural tests above.
add_executable(TextBlockAllocTest
  TextBlockAllocTest.cpp
  ${TEXTBLOCK_TEST_SOURCES}
)

target_include_directories(TextBlockAllocTest PRIVATE ${TEXTBLOCK_TEST_INCLUDES})

target_link_libraries(TextBlockAllocTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TextBlockAllocTest)
