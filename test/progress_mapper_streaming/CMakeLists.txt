add_executable(ProgressMapperStreamingTest
  ProgressMapperStreamingTest.cpp
  stubs/ChapterXPathResolverStub.cpp
  ${REPO_ROOT}/lib/KOReaderSync/ProgressMapper.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(ProgressMapperStreamingTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/KOReaderSync
)

target_link_libraries(ProgressMapperStreamingTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ProgressMapperStreamingTest)
