foreach(profile C3 S3)
  add_executable(FontRegistry${profile}Test
    FontRegistryTest.cpp
    ${REPO_ROOT}/lib/EpdFont/SdCardFontRegistry.cpp
  )
  target_include_directories(FontRegistry${profile}Test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../library_builder/stubs
    ${REPO_ROOT}/lib/EpdFont
  )
  if(profile STREQUAL "S3")
    target_compile_definitions(FontRegistry${profile}Test PRIVATE BOARD_HAS_PSRAM=1)
  endif()
  target_link_libraries(FontRegistry${profile}Test PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(FontRegistry${profile}Test TEST_PREFIX ${profile}.)
endforeach()
