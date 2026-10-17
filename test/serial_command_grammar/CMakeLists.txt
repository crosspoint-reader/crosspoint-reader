add_executable(SerialCommandGrammarTest
  SerialCommandGrammarTest.cpp
  ${REPO_ROOT}/src/util/SerialCommandGrammar.cpp
)

target_include_directories(SerialCommandGrammarTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SerialCommandGrammarTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerialCommandGrammarTest)
