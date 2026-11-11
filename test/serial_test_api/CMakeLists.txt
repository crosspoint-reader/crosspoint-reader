add_executable(SerialTestApiTest
  SerialCommandGrammarTest.cpp
  IdleGateTest.cpp
  ButtonPressQueueTest.cpp
  ${REPO_ROOT}/src/util/SerialCommandGrammar.cpp
)

target_include_directories(SerialTestApiTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SerialTestApiTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SerialTestApiTest)
