add_executable(TerminalTest
  TerminalTest.cpp
  Ssd1677Mode2StateTest.cpp
  ${REPO_ROOT}/src/terminal/TerminalDiagnostics.cpp
  ${REPO_ROOT}/src/terminal/TerminalProtocol.cpp
  ${REPO_ROOT}/src/terminal/TerminalScreen.cpp
  ${REPO_ROOT}/src/terminal/TerminalParser.cpp
)

target_include_directories(TerminalTest PRIVATE
  ${REPO_ROOT}/src/terminal
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/src/driver
)

target_link_libraries(TerminalTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TerminalTest)
