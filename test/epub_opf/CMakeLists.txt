add_executable(ContentOpfParserTest
  ContentOpfParserTest.cpp
  MetadataSidecarReadTest.cpp
  stubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/MetadataSidecar.cpp
  ${REPO_ROOT}/lib/FsHelpers/SidecarFiles.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
)

target_include_directories(ContentOpfParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Memory  # Memory.h (makeUniqueNoThrow) — matches the device per-lib include path
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  # The esp_heap_caps.h host stub the spine idref table (PassTable) sizes itself from.
  ${REPO_ROOT}/test/epub_pipeline
)

target_link_libraries(ContentOpfParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(ContentOpfParserTest)
