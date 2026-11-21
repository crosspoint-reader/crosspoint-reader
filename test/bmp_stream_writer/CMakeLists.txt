add_executable(BmpStreamWriterTest
  BmpStreamWriterTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BmpStreamWriter.cpp
)

target_include_directories(BmpStreamWriterTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(BmpStreamWriterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BmpStreamWriterTest)
