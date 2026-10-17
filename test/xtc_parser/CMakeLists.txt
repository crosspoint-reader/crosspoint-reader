add_executable(XtcParserTest
  XtcParserTest.cpp
  ${REPO_ROOT}/lib/Xtc/Xtc/XtcParser.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(XtcParserTest PRIVATE
  # Real stdio-backed FsFile/Storage (shadows test/shims/HalStorage.h).
  ${REPO_ROOT}/test/zip_entry_reader
  # Arduino shims: Print.h, HardwareSerial.h, WString.h.
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Xtc/Xtc
)

target_link_libraries(XtcParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XtcParserTest)
