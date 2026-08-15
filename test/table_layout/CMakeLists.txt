add_executable(TableLayoutTest
  TableLayoutTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/TableLayout.cpp
)

target_link_libraries(TableLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(TableLayoutTest PRIVATE
  ${REPO_ROOT}/lib/Epub
)

gtest_discover_tests(TableLayoutTest)
