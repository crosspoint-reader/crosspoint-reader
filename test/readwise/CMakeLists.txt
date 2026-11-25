add_executable(ReadwiseTest
  ReadwiseTest.cpp
  ${REPO_ROOT}/lib/Readwise/XhtmlSanitizer.cpp
  ${REPO_ROOT}/lib/Readwise/JsonStringStreamer.cpp
  ${REPO_ROOT}/lib/Readwise/EpubBuilder.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(ReadwiseTest PRIVATE
  ${REPO_ROOT}/lib/Readwise
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(ReadwiseTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(ReadwiseTest)
