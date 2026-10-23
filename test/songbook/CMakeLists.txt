FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(SongbookTest
  ChordProParserTest.cpp
  SongLayoutTest.cpp
  AutoScrollControllerTest.cpp
  SetlistTest.cpp
  SampleContentTest.cpp
  ChordNameTest.cpp
  ChordShapesTest.cpp
  SongChordsTest.cpp
  ${REPO_ROOT}/src/activities/songbook/ChordProParser.cpp
  ${REPO_ROOT}/src/activities/songbook/SongLayout.cpp
  ${REPO_ROOT}/src/activities/songbook/AutoScrollController.cpp
  ${REPO_ROOT}/src/activities/songbook/Setlist.cpp
  ${REPO_ROOT}/src/activities/songbook/ChordName.cpp
  ${REPO_ROOT}/src/activities/songbook/ChordShapes.cpp
  ${REPO_ROOT}/src/activities/songbook/SongChords.cpp
  ${REPO_ROOT}/src/activities/songbook/DiagramLayout.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(SongbookTest PRIVATE
  ${REPO_ROOT}/src/activities/songbook
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
)

target_compile_definitions(SongbookTest PRIVATE
  SONGBOOK_SAMPLE_DIR="${REPO_ROOT}/docs/songbook/sd-card/songbook"
)

target_link_libraries(SongbookTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(SongbookTest)
