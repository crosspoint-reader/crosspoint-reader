# The top-level test project enables only CXX; expat's sources are C, so turn on
# the C language here (co-located with the only C target in the test tree).
enable_language(C)

# expat compiled as an isolated host library so its C sources are not subject to
# the strict -Wall/-Wextra/-pedantic options applied to project test code.
add_library(toc_nav_expat STATIC
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmltok.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
)
target_include_directories(toc_nav_expat PUBLIC ${REPO_ROOT}/lib/expat)
target_compile_definitions(toc_nav_expat PUBLIC
  XML_GE=0
  XML_CONTEXT_BYTES=1024
  XML_POOR_ENTROPY  # host test; entropy source is irrelevant here
)

add_executable(TocNavParserTest
  TocNavParserTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/TocNavParser.cpp
)

target_include_directories(TocNavParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/shims  # host stand-ins for Arduino/Epub deps
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Epub/Epub/parsers
)

target_link_libraries(TocNavParserTest PRIVATE
  toc_nav_expat
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TocNavParserTest)
