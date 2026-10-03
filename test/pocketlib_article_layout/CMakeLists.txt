# Pocket Library: real Wikipedia articles through the cleaner and CrossPoint's
# layout engine, exactly as ArticleActivity drives them on the device.
# SPDX-License-Identifier: GPL-3.0-or-later

FetchContent_Declare(pocketlib_zim_testing_suite
  GIT_REPOSITORY https://github.com/openzim/zim-testing-suite.git
  GIT_TAG 2edf72096208e60c82b6ebbe313baa552cc6af52)
FetchContent_MakeAvailable(pocketlib_zim_testing_suite)

set(PARSER_TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../chapter_html_slim_parser)
set(ZIM_DIR ${REPO_ROOT}/lib/zim)

add_executable(PocketlibArticleLayoutTest
  ArticleLayoutTest.cpp
  ${PARSER_TEST_DIR}/ParserLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Page.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
  ${REPO_ROOT}/lib/Epub/Epub/ParsedText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/WordStore.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageDimsProbe.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${ZIM_DIR}/src/ZimSource.cpp
  ${ZIM_DIR}/src/ZimArchive.cpp
  ${ZIM_DIR}/src/ZimDecompress.cpp
  ${ZIM_DIR}/src/ZimHtml.cpp
  ${ZIM_DIR}/host/PosixSource.cpp
  ${ZIM_DIR}/src/third_party/zstd/zstddeclib.c
  ${ZIM_DIR}/src/third_party/xz/xz_crc32.c
  ${ZIM_DIR}/src/third_party/xz/xz_crc64.c
  ${ZIM_DIR}/src/third_party/xz/xz_dec_lzma2.c
  ${ZIM_DIR}/src/third_party/xz/xz_dec_stream.c
)

target_include_directories(PocketlibArticleLayoutTest PRIVATE
  ${PARSER_TEST_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/XmlParserUtils
  ${ZIM_DIR}/src
  ${ZIM_DIR}/src/third_party/xz
  ${ZIM_DIR}/host
)

set_source_files_properties(${ZIM_DIR}/src/third_party/zstd/zstddeclib.c PROPERTIES COMPILE_DEFINITIONS ZSTD_DISABLE_ASM=1)
target_compile_definitions(PocketlibArticleLayoutTest PRIVATE
  ZIM_TEST_DATA_DIR="${pocketlib_zim_testing_suite_SOURCE_DIR}/data")

target_link_libraries(PocketlibArticleLayoutTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(PocketlibArticleLayoutTest)
