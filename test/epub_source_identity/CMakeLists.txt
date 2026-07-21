add_executable(EpubSourceIdentityTest
  EpubSourceIdentityTest.cpp
  ${REPO_ROOT}/lib/ZipFile/ZipFile.cpp
  ${REPO_ROOT}/lib/Epub/Epub/BookMetadataCache.cpp
  ${REPO_ROOT}/lib/Epub/Epub/SourceIdentityStore.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(EpubSourceIdentityTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(EpubSourceIdentityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(EpubSourceIdentityTest)
