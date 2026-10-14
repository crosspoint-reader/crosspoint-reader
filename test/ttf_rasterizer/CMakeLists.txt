# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/third_party/freetype/ftgrays.c
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(TtfRasterizerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/EpdFont/ttf
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/test/shims
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

# Spec 9.3: the engine runs under AddressSanitizer and UBSan (CI's Linux GCC). Where ASan cannot link
# (MinGW), UBSan still traps on undefined behaviour without its runtime. FreeType is not instrumented.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=address,undefined")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=address,undefined)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_ASAN)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=undefined -fsanitize-undefined-trap-on-error")
set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
check_cxx_source_compiles("int main() { return 0; }" TTF_TESTS_HAVE_UBSAN_TRAP)
unset(CMAKE_REQUIRED_FLAGS)
unset(CMAKE_REQUIRED_LINK_OPTIONS)
if(TTF_TESTS_HAVE_ASAN)
  set(TTF_SANITIZERS -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
elseif(TTF_TESTS_HAVE_UBSAN_TRAP)
  set(TTF_SANITIZERS -fsanitize=undefined -fsanitize-undefined-trap-on-error)
endif()
target_compile_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
target_link_options(TtfRasterizerTest PRIVATE ${TTF_SANITIZERS})
message(STATUS "TtfRasterizerTest sanitizers: ${TTF_SANITIZERS}")

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
