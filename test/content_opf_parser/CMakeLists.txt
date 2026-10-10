find_package(EXPAT REQUIRED)

add_executable(ContentOpfParserTest
  ContentOpfParserTest.cpp
  EncryptionManifestProbeTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
)

target_include_directories(ContentOpfParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(ContentOpfParserTest PRIVATE
  crosspoint_test_common
  EXPAT::EXPAT
  GTest::gtest_main
)

gtest_discover_tests(ContentOpfParserTest)
