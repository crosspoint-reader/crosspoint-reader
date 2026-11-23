foreach(variant IN ITEMS Default C3Enabled S3Model)
  if(variant STREQUAL "Default")
    set(target BmpToBmpTest)
    set(prefix "")
  else()
    set(target BitmapRow${variant}Test)
    set(prefix "${variant}.")
  endif()
  add_executable(${target} BitmapGrayRowTest.cpp BitmapPackedRowTest.cpp BitmapBgrRowTest.cpp
    ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
    ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
    ${REPO_ROOT}/lib/ImageOps/src/Indexed.cpp
    ${REPO_ROOT}/lib/ImageOps/src/Rgb24.cpp)
  target_include_directories(${target} PRIVATE
    ${REPO_ROOT}/test/chapter_html_slim_parser/stubs
    ${REPO_ROOT}/lib/GfxRenderer
    ${REPO_ROOT}/lib/ImageOps/include)
  target_compile_definitions(${target} PRIVATE BMP_ROW_TEST_VARIANT="${variant}")
  if(variant STREQUAL "Default")
    target_sources(${target} PRIVATE BmpToBmpTest.cpp ${REPO_ROOT}/lib/BmpToBmpConverter/BmpToBmpConverter.cpp)
    target_include_directories(${target} BEFORE PRIVATE ${REPO_ROOT}/test/gif_support/stubs)
    target_include_directories(${target} PRIVATE
      ${REPO_ROOT}/lib/BmpToBmpConverter
      ${REPO_ROOT}/lib/GifSupport
      ${REPO_ROOT}/lib/Memory)
  elseif(variant STREQUAL "C3Enabled")
    target_compile_definitions(${target} PRIVATE CONFIG_IDF_TARGET_ESP32S3=0
      CROSSPOINT_ACCELERATED_IMAGES=1 BMP_ROW_TEST_FULL)
  else()
    target_compile_definitions(${target} PRIVATE CONFIG_IDF_TARGET_ESP32S3=1)
    target_sources(${target} PRIVATE ${REPO_ROOT}/test/image_ops/indexed_model.cpp
      ${REPO_ROOT}/test/image_ops/rgb24_model.cpp)
  endif()
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${target} TEST_PREFIX "${prefix}")
endforeach()
