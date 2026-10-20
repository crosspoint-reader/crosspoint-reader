add_executable(BookMetadataCacheTest
  BookMetadataCacheTest.cpp
  BookBinSizeLookupTest.cpp
  AllocationRefusal.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
)

target_include_directories(BookMetadataCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  # esp_heap_caps.h / esp_system.h host stubs and the synthetic-book writers (shared with the
  # pipeline test).
  ${REPO_ROOT}/test/epub_pipeline
  # Real stdio-backed FsFile/Storage — must precede test/shims, whose
  # HalStorage.h is a no-op stub, and whose BookMetadataCache.h would shadow
  # the real header we are testing.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Memory  # Memory.h (makeUniqueNoThrow) — matches the device per-lib include path
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Logging
  # Arduino shims (Arduino.h, Print.h, WString.h). Listed LAST so its
  # BookMetadataCache.h stub never shadows the real header under test.
  ${REPO_ROOT}/test/shims
)

# Counts central-directory walks (ZipFile.h); firmware never defines it.
target_compile_definitions(BookMetadataCacheTest PRIVATE ZIP_FILE_SCAN_STATS)

target_link_libraries(BookMetadataCacheTest PRIVATE
  crosspoint_test_common
  # ZipFile.cpp pulls in InflateReader/uzlib; reuse the host libs the
  # zip_entry_reader test already defines rather than recompiling them here.
  inflate_reader_host
  GTest::gtest_main
)

gtest_discover_tests(BookMetadataCacheTest)
