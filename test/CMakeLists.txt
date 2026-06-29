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

# Automatically regenerate test dictionaries if JSON sources/generator script change, or if files are missing.
file(GLOB DICTIONARY_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/data/dictionary-sources/*.json")

set(GENERATED_DICTS
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/en-es/en-es.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/english-full/english-full.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/english-no-syn/english-no-syn.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/fuzzy-fpi/fuzzy-fpi.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/html-definitions/html-definitions.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/ipa-phonetic/ipa-phonetic.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/macos-metadata/macos-metadata.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/multi-idx/multi-idx.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/multi-ifo/multi-ifo.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/no-ifo/no-ifo.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/only-dict/only-dict.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/overflow-fields/overflow-fields.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/phrase/phrase.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-fpi-prefix-collision/prep-fpi-prefix-collision.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-extract-dict/prep-extract-dict.dict.dz"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-fail-decompress/prep-fail-decompress.dict.dz"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-gen-idx/prep-gen-idx.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-gen-syn/prep-gen-syn.dict"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-long/prep-long.dict.dz"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-mini/prep-mini.dict.dz"
  "${CMAKE_CURRENT_SOURCE_DIR}/dictionaries/prep-syn-two-step/prep-syn-two-step.dict"
)

add_custom_command(
  OUTPUT ${GENERATED_DICTS}
  COMMAND python3 "${CMAKE_CURRENT_SOURCE_DIR}/data/generate_dictionaries.py" --all >/dev/null
  DEPENDS ${DICTIONARY_SOURCES} "${CMAKE_CURRENT_SOURCE_DIR}/data/generate_dictionaries.py"
  COMMENT "Regenerating test dictionaries..."
)

add_custom_target(generate_test_dictionaries ALL
  DEPENDS ${GENERATED_DICTS}
)

enable_testing()
include(GoogleTest)

add_subdirectory(streaming_json_parser)
add_subdirectory(release_json_parser)
add_subdirectory(differential_rounding)
add_subdirectory(hyphenation_eval)
add_subdirectory(utf8_compose)
add_subdirectory(dict-engine)
add_subdirectory(dict-layout)
add_subdirectory(dict-html-renderer)
add_subdirectory(dict-word-select-nav)
