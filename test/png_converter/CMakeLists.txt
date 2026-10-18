add_executable(PngConverterTest PngConverterTest.cpp
  ${REPO_ROOT}/lib/PngToBmpConverter/PngToBmpConverter.cpp ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp ${REPO_ROOT}/lib/hal/HalMemory.cpp)
target_include_directories(PngConverterTest PRIVATE stubs ../zip_file/stubs
  ${REPO_ROOT}/lib/PngToBmpConverter ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/hal ${REPO_ROOT}/lib/miniz/src)
target_link_libraries(PngConverterTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PngConverterTest)
