# Declaration-level parser tests: inline-style and declaration parsing, no stylesheet
# cache. Pure host code, so they run in the default ctest sweep on any host.
add_executable(CssParserTest
  CssParserTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

target_include_directories(CssParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(CssParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CssParserTest)

# Shims (HalStorage.h, LoggingStub.cpp with uzlib checksum stubs) are shared
# with the ZipEntryReaderTest harness — keep them in test/zip_entry_reader.
add_executable(EpubCssCacheTest
  CssParserCacheTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

target_include_directories(EpubCssCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(EpubCssCacheTest PRIVATE
  FIXTURE_EPUB="${REPO_ROOT}/test/fixtures/test_large_css.epub"
)

target_link_libraries(EpubCssCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubCssCacheTest)

# The cascade (which matching rule wins), resolved through every store the resolver serves from:
# an in-RAM parse, the compiled cache through the heap index, and both arena layouts.
add_executable(EpubCssCascadeTest
  CssCascadeTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(EpubCssCascadeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_link_libraries(EpubCssCascadeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubCssCascadeTest)

# Selector census over the EPUBs the tests ship: how common each kind of selector is, and how many
# the resolver can use. Prints CSS_CENSUS lines; see CssCensusTest.cpp.
add_executable(EpubCssCensusTest
  CssCensusTest.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

target_include_directories(EpubCssCensusTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(EpubCssCensusTest PRIVATE
  CORPUS_DIR="${REPO_ROOT}/test/epubs"
  MOBY_DICK_EPUB="${REPO_ROOT}/test/fixtures/moby-dick.epub"
)

target_link_libraries(EpubCssCensusTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubCssCensusTest)
