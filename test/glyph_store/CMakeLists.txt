add_executable(GlyphStoreTest
  SimNorFlashTest.cpp
)

target_include_directories(GlyphStoreTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(GlyphStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(GlyphStoreTest)
