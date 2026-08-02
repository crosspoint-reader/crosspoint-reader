add_executable(QuizFormatReaderTest
  quiz_format_reader_test.cpp
  ${REPO_ROOT}/src/activities/quiz/core/QuizFormatReader.cpp
)
target_include_directories(QuizFormatReaderTest PRIVATE ${REPO_ROOT}/src/activities/quiz/core)
target_compile_definitions(QuizFormatReaderTest PRIVATE QUIZ_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(QuizFormatReaderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(QuizFormatReaderTest)
