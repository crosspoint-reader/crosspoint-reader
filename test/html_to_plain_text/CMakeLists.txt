add_executable(HtmlToPlainTextTest
  HtmlToPlainTextTest.cpp
  ${REPO_ROOT}/src/util/HtmlToPlainText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(HtmlToPlainTextTest PRIVATE
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(HtmlToPlainTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HtmlToPlainTextTest)
