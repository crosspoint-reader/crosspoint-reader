add_executable(FlashBlobCacheTest
  FlashBlobCacheTest.cpp
  ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp
)

target_include_directories(FlashBlobCacheTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/EpdFont
)

# The test supplies the partition backend (see FlashBlobCacheTest.cpp).
target_compile_definitions(FlashBlobCacheTest PRIVATE FLASH_BLOB_CACHE_EXTERNAL_BACKEND)

target_link_libraries(FlashBlobCacheTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FlashBlobCacheTest)
