add_executable(TypographyTest
  TypographyTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/CjkPunctuation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/TypographyProfile.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(TypographyTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(TypographyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TypographyTest)
