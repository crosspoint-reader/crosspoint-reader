find_package(ZLIB REQUIRED)

foreach(target IN ITEMS PngToBmpTest PngToBmpScalarTest)
  add_executable(${target} PngToBmpTest.cpp
    ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp
    ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
    ${REPO_ROOT}/lib/GfxRenderer/BmpStreamWriter.cpp
    ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
    ${REPO_ROOT}/lib/hal/HalMemory.cpp
    ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp
    ${REPO_ROOT}/lib/miniz/src/miniz_impl.c)
  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
    ${REPO_ROOT}/test/gif_support/stubs
    ${REPO_ROOT}/test/inflate_stream/stubs
    ${REPO_ROOT}/lib/PngToBmpConverter
    ${REPO_ROOT}/lib/GfxRenderer
    ${REPO_ROOT}/lib/ImageOps/include
    ${REPO_ROOT}/lib/Memory
    ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/lib/miniz/src)
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main ZLIB::ZLIB)
  if(target STREQUAL "PngToBmpTest")
    target_sources(${target} PRIVATE
      ${REPO_ROOT}/lib/ImageOps/src/Indexed.cpp
      ${REPO_ROOT}/lib/ImageOps/src/Rgb24.cpp)
    target_compile_definitions(${target} PRIVATE CROSSPOINT_ACCELERATED_IMAGES=1)
    gtest_discover_tests(${target})
  else()
    target_compile_definitions(${target} PRIVATE CROSSPOINT_ACCELERATED_IMAGES=0)
    gtest_discover_tests(${target} TEST_PREFIX "Scalar.")
  endif()
endforeach()
