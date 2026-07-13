# Fixture: resources/mini.cpd is generated from resources/src/mini.index with
#   python scripts/make_dictionary.py test/dictionary/resources/src/mini.index \
#     -o test/dictionary/resources/mini.cpd --turkish-keys --lang tr-en \
#     --title "Mini Test Dictionary"
add_executable(DictionaryStoreTest
  DictionaryStoreTest.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(DictionaryStoreTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(DictionaryStoreTest PRIVATE
  DICTIONARY_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(DictionaryStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryStoreTest)
