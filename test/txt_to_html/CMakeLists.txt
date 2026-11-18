add_executable(TxtToHtmlTest
  TxtToHtmlTest.cpp
  ${REPO_ROOT}/lib/Txt/TxtToHtml.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TxtToHtmlTest PRIVATE
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Txt
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/fs_helpers/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
)

target_link_libraries(TxtToHtmlTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TxtToHtmlTest)
