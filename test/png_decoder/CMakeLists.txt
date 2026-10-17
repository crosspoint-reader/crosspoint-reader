add_executable(PngDecoderArenaTest
  PngDecoderArenaTest.cpp
  PngDecoderIhdrTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/PngDecoder/PngStreamDecoder.cpp
)

target_include_directories(PngDecoderArenaTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Real stdio-backed FsFile/Storage (shadows the test/shims HalStorage.h stub).
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims           # Print.h, HardwareSerial.h, WString.h
  ${REPO_ROOT}/lib/PngDecoder
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PngDecoderArenaTest PRIVATE
  crosspoint_test_common
  inflate_reader_host
  GTest::gtest_main
)

target_compile_definitions(PngDecoderArenaTest PRIVATE
  FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)

gtest_discover_tests(PngDecoderArenaTest)
