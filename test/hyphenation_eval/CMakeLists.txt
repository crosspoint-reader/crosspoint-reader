add_executable(HyphenationEvaluationTest
  HyphenationEvaluationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(HyphenationEvaluationTest PRIVATE
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(HyphenationEvaluationTest PRIVATE
  HYPHENATION_RESOURCES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/resources"
)

target_link_libraries(HyphenationEvaluationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(HyphenationEvaluationTest)
