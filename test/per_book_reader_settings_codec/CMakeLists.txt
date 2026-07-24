add_executable(PerBookReaderSettingsCodecTest
  PerBookReaderSettingsCodecTest.cpp
)

target_include_directories(PerBookReaderSettingsCodecTest PRIVATE
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(PerBookReaderSettingsCodecTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PerBookReaderSettingsCodecTest)

add_executable(PerBookReaderSettingsStoreTest
  PerBookReaderSettingsStoreTest.cpp
  ${REPO_ROOT}/src/activities/reader/PerBookReaderSettingsStore.cpp
)

target_include_directories(PerBookReaderSettingsStoreTest PRIVATE
  ${REPO_ROOT}/test/clippings_core/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(PerBookReaderSettingsStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PerBookReaderSettingsStoreTest)
