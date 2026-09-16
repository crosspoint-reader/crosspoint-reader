add_executable(GfxSpaceTest
  GfxSpaceTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(GfxSpaceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/sd_card_font/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MiniBidi
)
target_link_libraries(GfxSpaceTest PRIVATE crosspoint_test_common GTest::gtest_main)
# Compile the complete renderer; discard hardware-only entry points at link time.
target_compile_options(GfxSpaceTest PRIVATE -fno-exceptions -ffunction-sections -fdata-sections)
if(APPLE)
  target_link_options(GfxSpaceTest PRIVATE -Wl,-dead_strip)
else()
  target_link_options(GfxSpaceTest PRIVATE -Wl,--gc-sections)
endif()
gtest_discover_tests(GfxSpaceTest)
