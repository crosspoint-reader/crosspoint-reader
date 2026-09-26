find_package(EXPAT REQUIRED)

# Indexes books with thousands of chapters under a heap cap: the real OPF, nav
# and book.bin passes over an in-memory SD card, with every operator new counted.
add_executable(HugeBookIndexTest
  HugeBookIndexTest.cpp
  HeapCap.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
)

target_include_directories(HugeBookIndexTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(HugeBookIndexTest PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(HugeBookIndexTest)
