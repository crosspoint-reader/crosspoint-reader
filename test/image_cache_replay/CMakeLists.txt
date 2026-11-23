# Match the firmware decoder pins, but build only their unmodified host sources.
FetchContent_Declare(pngdec
  GIT_REPOSITORY https://github.com/bitbank2/PNGdec.git
  GIT_TAG 9a9c585fd39d148c5517597b02ce490e0fdb1bb4
  SOURCE_SUBDIR src)
FetchContent_Declare(jpegdec
  GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git
  GIT_TAG 86282979224c8a32fd51e091ed5a35b0c699a52b
  SOURCE_SUBDIR src)
if(CMAKE_VERSION VERSION_LESS 3.18)
  FetchContent_Populate(pngdec)
  FetchContent_Populate(jpegdec)
else()
  FetchContent_MakeAvailable(pngdec jpegdec)
endif()

set(png_src "${pngdec_SOURCE_DIR}/src")
set(jpeg_src "${jpegdec_SOURCE_DIR}/src")
add_library(image_decoders_host STATIC
  ${png_src}/PNGdec.cpp
  ${png_src}/adler32.c
  ${png_src}/crc32.c
  ${png_src}/inffast.c
  ${png_src}/inflate.c
  ${png_src}/inftrees.c
  ${png_src}/zutil.c
  ${jpeg_src}/JPEGDEC.cpp)
target_include_directories(image_decoders_host SYSTEM PUBLIC ${png_src} ${jpeg_src})
target_include_directories(image_decoders_host PRIVATE stubs)
target_compile_definitions(image_decoders_host PUBLIC __LINUX__ PNG_MAX_BUFFERED_PIXELS=16416)

set(converters "${REPO_ROOT}/lib/Epub/Epub/converters")
set(image_test_includes
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/GifSupport
  ${REPO_ROOT}/lib/ImageOps/include
  ${REPO_ROOT}/lib/Serialization)

# Link the disabled converters alongside the enabled ones without changing firmware APIs.
add_library(image_scalar_converters STATIC ScalarImageDecoders.cpp
  ${converters}/PngToFramebufferConverter.cpp
  ${converters}/JpegToFramebufferConverter.cpp)
target_include_directories(image_scalar_converters PRIVATE ${image_test_includes})
target_compile_definitions(image_scalar_converters PRIVATE CROSSPOINT_ACCELERATED_IMAGES=0
  PngToFramebufferConverter=ScalarPngToFramebufferConverter
  JpegToFramebufferConverter=ScalarJpegToFramebufferConverter)
target_link_libraries(image_scalar_converters PRIVATE crosspoint_test_common PUBLIC image_decoders_host)

find_package(ZLIB REQUIRED)
add_executable(ImageCacheReplayTest ImageCacheReplayTest.cpp ImageScalingTest.cpp ImagePackedWriterTest.cpp
  ${REPO_ROOT}/test/gif_support/GifFramebufferTest.cpp
  ${converters}/ImageDecoderFactory.cpp
  ${converters}/ImageToFramebufferDecoder.cpp
  ${converters}/PngToFramebufferConverter.cpp
  ${converters}/JpegToFramebufferConverter.cpp
  ${converters}/GifToFramebufferConverter.cpp
  ${converters}/PortraitImageRows.cpp
  ${converters}/PortraitImageWriter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/ImageBlock.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/ImagePackedWriter.cpp
  ${REPO_ROOT}/lib/ImageOps/src/BayerDither.cpp
  ${REPO_ROOT}/lib/ImageOps/src/Indexed.cpp
  ${REPO_ROOT}/lib/ImageOps/src/Rgb24.cpp
  ${REPO_ROOT}/lib/ImageOps/src/TilePlanes.cpp)
target_include_directories(ImageCacheReplayTest PRIVATE ${image_test_includes})
target_compile_definitions(ImageCacheReplayTest PRIVATE CROSSPOINT_ACCELERATED_IMAGES=1)
target_link_libraries(ImageCacheReplayTest PRIVATE
  crosspoint_test_common GTest::gtest_main animatedgif_host image_scalar_converters ZLIB::ZLIB)
gtest_discover_tests(ImageCacheReplayTest)
