add_executable(FileBrowserModelTest
  FileBrowserModelTest.cpp
  ${REPO_ROOT}/src/activities/home/FileBrowserModel.cpp
  ${REPO_ROOT}/src/activities/home/FolderSearch.cpp
  ${REPO_ROOT}/src/activities/home/FolderCountMemo.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBlob.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexReader.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(FileBrowserModelTest PRIVATE
  # The fake FileIndex.h, ahead of everything: the real one keeps its index under /.crosspoint.
  ${CMAKE_CURRENT_SOURCE_DIR}
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  # I18n.h stub: tr() gives the key's name.
  ${REPO_ROOT}/test/epub_pipeline
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/home
  ${REPO_ROOT}/lib/CooperativeAbort
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(FileBrowserModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FileBrowserModelTest)
