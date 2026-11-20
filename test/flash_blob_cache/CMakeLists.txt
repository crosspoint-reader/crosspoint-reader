# One test binary per fake "spiffs" partition size: CrossPoint's layout (8
# slots), a stock Xteink 1 MB partition (7) and one too small for two slots.
foreach(variant IN ITEMS "2MB;0x200000;8" "1MB;0x100000;7" "256KB;0x40000;0")
  list(GET variant 0 name)
  list(GET variant 1 size)
  list(GET variant 2 slots)
  set(target FlashBlobCacheTest${name})
  add_executable(${target} FlashBlobCacheTest.cpp ${REPO_ROOT}/lib/EpdFont/FlashBlobCache.cpp)
  target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/lib/EpdFont)
  # The test supplies the partition backend (see FlashBlobCacheTest.cpp).
  target_compile_definitions(${target} PRIVATE FLASH_BLOB_CACHE_EXTERNAL_BACKEND FAKE_FLASH_SIZE=${size}u
                             EXPECTED_SLOTS=${slots})
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${target} TEST_PREFIX "${name}.")
endforeach()
