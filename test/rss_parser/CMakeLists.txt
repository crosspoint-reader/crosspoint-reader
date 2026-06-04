find_package(EXPAT REQUIRED)

add_executable(RssParserTest
  RssParserTest.cpp
  ${REPO_ROOT}/lib/RssParser/RssParser.cpp
)

target_include_directories(RssParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/RssParser
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(RssParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  EXPAT::EXPAT
)

gtest_discover_tests(RssParserTest)
