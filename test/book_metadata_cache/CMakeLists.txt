add_executable(BookMetadataCacheTest BookMetadataCacheTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(BookMetadataCacheTest PRIVATE stubs ../library_builder/stubs
  ${REPO_ROOT}/lib/Epub/Epub ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8)
target_link_libraries(BookMetadataCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookMetadataCacheTest)
