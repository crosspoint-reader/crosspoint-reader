add_executable(ReaderSessionTest
  ReaderSessionTest.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderSession.cpp
)

target_include_directories(ReaderSessionTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(ReaderSessionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReaderSessionTest)
