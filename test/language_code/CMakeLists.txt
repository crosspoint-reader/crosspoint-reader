add_executable(LanguageCodeTest LanguageCodeTest.cpp)

target_link_libraries(LanguageCodeTest PRIVATE
  crosspoint_test_common
  crosspoint_language_code
  GTest::gtest_main
)

gtest_discover_tests(LanguageCodeTest)
