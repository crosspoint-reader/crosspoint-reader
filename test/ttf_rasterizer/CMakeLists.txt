# The TTF engine (lib/EpdFont/ttf): FontBytes, face parsing, the integer TrueType loader and the vendored
# ftgrays, checked against FreeType itself over every bundled TTF.

# FreeType for the comparison only, never the firmware: the SDK's copy, compiled through its own build
# wrappers as FreeInkFont's host tests compile it. Third-party code, so its warnings are not ours.
set(FREEINK_FONT ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB FREETYPE_HOST_SOURCES ${FREEINK_FONT}/src/freetype/ft_*.c)
add_library(freetype_host STATIC ${FREETYPE_HOST_SOURCES})
target_include_directories(freetype_host PUBLIC ${FREEINK_FONT}/third_party/freetype/include)
target_compile_options(freetype_host PRIVATE -w)

add_executable(TtfRasterizerTest
  TtfGraysTest.cpp
  FontBytesTest.cpp
  TtfFixedTest.cpp
  TtfFaceTest.cpp
  TtfRasterizerTest.cpp
  TtfVsFreeTypeTest.cpp
  TtfCorruptFontTest.cpp
  FreeTypeReference.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfFace.cpp
  ${REPO_ROOT}/lib/EpdFont/ttf/TtfRasterizer.cpp
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
  ${REPO_ROOT}/bench
)

target_compile_definitions(TtfRasterizerTest PRIVATE
  TTF_FONT_DIR="${REPO_ROOT}/lib/EpdFont/builtinFonts/source"
)

target_link_libraries(TtfRasterizerTest PRIVATE
  crosspoint_test_common
  freetype_host
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
set(TTF_SANITIZERS ${TTF_SANITIZERS} PARENT_SCOPE)  # ttf_font uses them too

gtest_discover_tests(TtfRasterizerTest DISCOVERY_TIMEOUT 60)
