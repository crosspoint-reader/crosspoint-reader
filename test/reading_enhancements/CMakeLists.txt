add_executable(ReadingEnhancementsTest
  ReadingEnhancementsTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/EpubSearch.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(ReadingEnhancementsTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
)

target_link_libraries(ReadingEnhancementsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ReadingEnhancementsTest)
