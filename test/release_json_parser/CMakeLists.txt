add_executable(ReleaseJsonParserTest
  ReleaseJsonParserTest.cpp
  ${REPO_ROOT}/lib/JsonParser/ReleaseJsonParser.cpp
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp
)

target_include_directories(ReleaseJsonParserTest PRIVATE
  ${REPO_ROOT}/lib/JsonParser
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
)

target_link_libraries(ReleaseJsonParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReleaseJsonParserTest)
