find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ANIMATEDGIF_SOURCE_DIR "" CACHE PATH "Existing pinned AnimatedGIF source for host tests")
if(NOT ANIMATEDGIF_SOURCE_DIR)
  FetchContent_Declare(animatedgif
    GIT_REPOSITORY https://github.com/bitbank2/AnimatedGIF.git
    GIT_TAG 67cfacafe12ef4926c7713342c57d2a4af040e5e)
  FetchContent_MakeAvailable(animatedgif)
  set(ANIMATEDGIF_SOURCE_DIR "${animatedgif_SOURCE_DIR}")
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/patch_animatedgif.py ${ANIMATEDGIF_SOURCE_DIR}
  RESULT_VARIABLE gif_patch_result)
if(NOT gif_patch_result EQUAL 0)
  message(FATAL_ERROR "AnimatedGIF host dependency patch failed")
endif()
add_library(animatedgif_host STATIC ${ANIMATEDGIF_SOURCE_DIR}/src/AnimatedGIF.cpp)
target_include_directories(animatedgif_host PUBLIC ${ANIMATEDGIF_SOURCE_DIR}/src)
target_compile_definitions(animatedgif_host PUBLIC __LINUX__ MAX_WIDTH=3072)

add_executable(GifSupportTest GifSupportTest.cpp PixelCacheTest.cpp GifDecoderTest.cpp)
target_include_directories(GifSupportTest PRIVATE
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs)
target_link_libraries(GifSupportTest PRIVATE crosspoint_test_common GTest::gtest_main animatedgif_host)
gtest_discover_tests(GifSupportTest)
add_test(NAME PatchAnimatedGif COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/test_patch_animatedgif.py)

add_executable(GifBmpTest GifBmpTest.cpp
  ${REPO_ROOT}/lib/GifToBmpConverter/GifToBmpConverter.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp)
target_include_directories(GifBmpTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
  ${REPO_ROOT}/lib/GifToBmpConverter
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Memory)
target_link_libraries(GifBmpTest PRIVATE crosspoint_test_common GTest::gtest_main animatedgif_host)
gtest_discover_tests(GifBmpTest)
