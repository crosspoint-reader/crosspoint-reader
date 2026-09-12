# The hook names affect only the resolver object. Expat itself remains real.
add_library(KOReaderProgressParserUnderTest OBJECT
  ${REPO_ROOT}/lib/KOReaderSync/ChapterXPathResolver.cpp
)
target_compile_definitions(KOReaderProgressParserUnderTest PRIVATE
  XML_ParserCreate=trackedXmlParserCreate
  XML_ParserFree=trackedXmlParserFree
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)
target_include_directories(KOReaderProgressParserUnderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/expat
)
target_link_libraries(KOReaderProgressParserUnderTest PRIVATE crosspoint_test_common)

add_executable(KOReaderParserLifetimeTest
  KOReaderParserLifetimeTest.cpp
  $<TARGET_OBJECTS:KOReaderProgressParserUnderTest>
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)
target_include_directories(KOReaderParserLifetimeTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/expat
)
target_compile_definitions(KOReaderParserLifetimeTest PRIVATE XML_GE=0 XML_CONTEXT_BYTES=1024)
target_link_libraries(KOReaderParserLifetimeTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(KOReaderParserLifetimeTest)
