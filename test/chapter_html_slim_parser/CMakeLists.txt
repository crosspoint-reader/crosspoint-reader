add_executable(ChapterHtmlSlimParserTest
  ChapterHtmlSlimParserTest.cpp
  GfxRendererTestDouble.cpp
  PageBlockTestDoubles.cpp
  stubs/HalStorage.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/Hyphenator.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LanguageRegistry.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/LiangHyphenation.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(ChapterHtmlSlimParserTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs  # HalStorage/HalDisplay/Logging/Arduino/Print/WString doubles - must win over any real lib/hal path
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/FsHelpers
)

target_compile_definitions(ChapterHtmlSlimParserTest PRIVATE
  XML_STATIC
  XML_GE=0
  XML_CONTEXT_BYTES=1024
)

target_link_libraries(ChapterHtmlSlimParserTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ChapterHtmlSlimParserTest)
