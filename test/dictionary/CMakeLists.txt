add_executable(DictionaryTest
  DictionaryTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/src/util/DictionaryRegistry.cpp
  ${REPO_ROOT}/src/util/DictZip.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(DictionaryTest PRIVATE
  ${REPO_ROOT}/test/zip_entry_reader   # HalStorage.h with real file + directory I/O
  ${REPO_ROOT}/test/shims              # Print.h, HardwareSerial.h, WString.h, Arduino.h
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Utf8
)

# The registry scans absolute SD roots (/dictionaries, /.dictionaries). Redirect
# both into the build tree so the test owns a writable copy -- sidecars are
# written next to the index, so the fixture cannot be read in place.
set(DICT_TEST_ROOT "${CMAKE_CURRENT_BINARY_DIR}/sdroot")
target_compile_definitions(DictionaryTest PRIVATE
  DICTIONARY_ROOT_PREFIX="${DICT_TEST_ROOT}"
  DICT_TEST_ROOT="${DICT_TEST_ROOT}"
  DICT_FIXTURE_DIR="${REPO_ROOT}/test/fixtures/dictionary"
)

target_link_libraries(DictionaryTest PRIVATE
  crosspoint_test_common
  inflate_reader_host
  GTest::gtest_main
)

# gtest_discover_tests runs every case as its own ctest process, and they all
# share the one SD root above -- which each process wipes and repopulates on
# entry. A resource lock serialises them so no run pulls the directory out from
# under another; the whole suite takes a few milliseconds, so nothing is lost.
gtest_discover_tests(DictionaryTest PROPERTIES RESOURCE_LOCK dictionary_sd_root)

# The HTML normalizer needs nothing but a Print sink, so it is covered on its
# own -- and against the real SaxParser, which is the only way to prove the
# repairs it deliberately leaves to the parser are actually made there.
add_executable(DictHtmlNormalizeTest
  DictHtmlNormalizeTest.cpp
  ${REPO_ROOT}/src/util/DictHtmlNormalize.cpp
)

target_include_directories(DictHtmlNormalizeTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src/util
)

target_link_libraries(DictHtmlNormalizeTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  SaxParser
)

gtest_discover_tests(DictHtmlNormalizeTest)

# The plain-text fallback is a pure string transform over the shared HTML entity
# table, so it needs neither storage nor a renderer.
add_executable(HtmlToPlainTextTest
  HtmlToPlainTextTest.cpp
  ${REPO_ROOT}/src/util/HtmlToPlainText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
)

target_include_directories(HtmlToPlainTextTest PRIVATE
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(HtmlToPlainTextTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HtmlToPlainTextTest)
