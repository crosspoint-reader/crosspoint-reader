cmake_minimum_required(VERSION 3.16)
project(crosspoint_reader_tests C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")

add_library(crosspoint_test_common INTERFACE)
target_include_directories(crosspoint_test_common INTERFACE
  ${REPO_ROOT}
  ${REPO_ROOT}/lib
)
target_compile_options(crosspoint_test_common INTERFACE
  -Wall
  -Wextra
  -pedantic
)

enable_testing()
include(GoogleTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(I18N_DIR ${REPO_ROOT}/lib/I18n)
file(GLOB I18N_TRANSLATIONS CONFIGURE_DEPENDS ${I18N_DIR}/translations/*.yaml)
add_custom_command(
  OUTPUT ${I18N_DIR}/I18nKeys.h ${I18N_DIR}/I18nStrings.h ${I18N_DIR}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py ${I18N_DIR}/translations ${I18N_DIR}/
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${I18N_TRANSLATIONS}
  WORKING_DIRECTORY ${REPO_ROOT}
  COMMENT "Generating I18n string tables"
  VERBATIM
)
add_library(crosspoint_i18n_tables STATIC ${I18N_DIR}/I18nStrings.cpp ${I18N_DIR}/I18nKeys.h)
target_include_directories(crosspoint_i18n_tables PUBLIC ${I18N_DIR})

add_library(crosspoint_language_code STATIC ${REPO_ROOT}/lib/LanguageCode/LanguageCode.cpp)
target_include_directories(crosspoint_language_code PUBLIC ${REPO_ROOT}/lib/LanguageCode PRIVATE ${REPO_ROOT}/lib/Utf8)
target_link_libraries(crosspoint_language_code PRIVATE crosspoint_test_common)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(language_code)
add_subdirectory(language_tag)
add_subdirectory(utf8_compose)
add_subdirectory(opds_filename)
add_subdirectory(html_to_plain_text)
add_subdirectory(library_text)
add_subdirectory(library_format)
add_subdirectory(library_index_file)
add_subdirectory(library_builder)
add_subdirectory(minibidi_arabic)
add_subdirectory(combining_marks)
add_subdirectory(credential_integrity)
add_subdirectory(xml_parser_utils)
add_subdirectory(token_boundary)
add_subdirectory(koreader_xpath_resolver)
add_subdirectory(progress_comparison)
add_subdirectory(font_cache_manager)
add_subdirectory(css_parser)
add_subdirectory(chapter_html_slim_parser)
add_subdirectory(korean_line_breaking)
add_subdirectory(ligature_guard)
add_subdirectory(page_link)
add_subdirectory(content_opf_parser)
add_subdirectory(chapter_position)
add_subdirectory(home_button)
add_subdirectory(button_press_navigation)

add_subdirectory(inflate_stream)
add_subdirectory(zip_file)
add_subdirectory(fs_helpers)
add_subdirectory(txt_to_html)

add_subdirectory(absolute_grayscale)

add_subdirectory(sd_card_font)
add_subdirectory(clipping_preview)
add_subdirectory(glyph_bitmap)

add_subdirectory(clipping_store)
add_subdirectory(reader_session)
add_subdirectory(plugin_manifest_classification)
add_subdirectory(protected_paths)

add_test(NAME TrustedTimeTimezone COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_trusted_time.py)
