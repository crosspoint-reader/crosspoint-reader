find_package(EXPAT REQUIRED)

add_executable(ChapterXPathResolverTest
  ChapterXPathResolverTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ChapterXPathResolverTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(ChapterXPathResolverTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  EXPAT::EXPAT
)

gtest_discover_tests(ChapterXPathResolverTest)
