set(EXPAT_SOURCES
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

set_source_files_properties(${EXPAT_SOURCES} PROPERTIES LANGUAGE C)

add_executable(DictHtmlRendererTest
  DictHtmlRendererTest.cpp
  ${REPO_ROOT}/lib/DictHtmlRenderer/DictHtmlRenderer.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${EXPAT_SOURCES}
)

target_include_directories(DictHtmlRendererTest PRIVATE
  ${REPO_ROOT}/test/stubs  # overrides HalStorage.h / Logging.h (device-only)
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/expat
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/src
)

target_compile_definitions(DictHtmlRendererTest PRIVATE
  XML_GE=0
  XML_CONTEXT_BYTES=1024
  DICT_HTML_RENDERER_TRACK_UNKNOWN
  DICT_HTML_TEST_DATA="${CMAKE_CURRENT_SOURCE_DIR}/../dictionaries/html-definitions"
)

target_link_libraries(DictHtmlRendererTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

add_dependencies(DictHtmlRendererTest generate_test_dictionaries)

gtest_discover_tests(DictHtmlRendererTest)
