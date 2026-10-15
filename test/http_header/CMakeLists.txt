add_executable(HttpHeaderTest
  HttpHeaderTest.cpp
  ${REPO_ROOT}/lib/HttpHeader/HttpHeader.cpp
)

target_include_directories(HttpHeaderTest PRIVATE
  ${REPO_ROOT}/lib/HttpHeader
  ${REPO_ROOT}/lib/AsciiText
)

target_link_libraries(HttpHeaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HttpHeaderTest)
