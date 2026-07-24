add_executable(DictionarySynonymTest
  DictionarySynonymTest.cpp
  ${REPO_ROOT}/src/util/StarDictSynonyms.cpp
  ${REPO_ROOT}/src/util/DictionaryHistoryStore.cpp
  ${REPO_ROOT}/src/util/DictionaryQuery.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictionarySynonymTest PRIVATE
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)
target_compile_definitions(DictionarySynonymTest PRIVATE UNIT_TEST)
target_link_libraries(DictionarySynonymTest PRIVATE
  GTest::gtest_main
  crosspoint_test_common
)

gtest_discover_tests(DictionarySynonymTest)
