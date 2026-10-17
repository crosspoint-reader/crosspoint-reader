enable_language(C)

add_library(uzlib_host STATIC
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)
target_include_directories(uzlib_host PUBLIC ${REPO_ROOT}/lib/uzlib/src)

add_library(inflate_reader_host STATIC
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
)
target_include_directories(inflate_reader_host PUBLIC
  ${REPO_ROOT}/lib/InflateReader
)
target_link_libraries(inflate_reader_host PUBLIC uzlib_host)

add_executable(ZipEntryReaderTest
  ZipEntryReaderTest.cpp
  ZipCorruptSizeTest.cpp
  LoggingStub.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
)

target_include_directories(ZipEntryReaderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}       # HalStorage.h (real I/O; shadows test/shims stub)
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims           # Print.h, HardwareSerial.h, WString.h
)

# FsShim.h overrides the Arduino HalStorage.h — put it first on the include path.
target_compile_definitions(ZipEntryReaderTest PRIVATE
  FIXTURE_EPUB="${REPO_ROOT}/test/fixtures/moby-dick.epub"
)

target_link_libraries(ZipEntryReaderTest PRIVATE
  crosspoint_test_common
  inflate_reader_host
  GTest::gtest_main
)

gtest_discover_tests(ZipEntryReaderTest)
