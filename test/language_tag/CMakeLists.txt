add_executable(LanguageTagTest
  LanguageTagTest.cpp
  ${REPO_ROOT}/lib/I18n/LanguageTag.cpp
)

target_include_directories(LanguageTagTest PRIVATE ${REPO_ROOT}/lib/Utf8)

target_link_libraries(LanguageTagTest PRIVATE
  crosspoint_test_common
  crosspoint_i18n_tables
  crosspoint_language_code
  GTest::gtest_main
)

gtest_discover_tests(LanguageTagTest)
