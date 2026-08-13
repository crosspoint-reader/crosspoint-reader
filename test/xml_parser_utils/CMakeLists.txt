add_executable(XmlParserUtilsTest
  XmlParserUtilsTest.cpp
  ContentOpfParserTest.cpp
  HalStorageStub.cpp
  BookMetadataCacheStub.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ContentOpfParser.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

# expat requires these compile-time switches (mirrors platformio.ini).
target_compile_definitions(XmlParserUtilsTest PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_include_directories(XmlParserUtilsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)

target_link_libraries(XmlParserUtilsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(XmlParserUtilsTest)
