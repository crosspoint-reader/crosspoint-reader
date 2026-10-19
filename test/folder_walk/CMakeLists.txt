add_executable(FolderWalkTest
  FolderWalkTest.cpp
  ${REPO_ROOT}/lib/FsHelpers/FolderWalk.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(FolderWalkTest PRIVATE
  # stdio-backed HalStorage.h; must shadow the test/shims stub, which cannot open files.
  ${REPO_ROOT}/test/zip_entry_reader
  ${REPO_ROOT}/test/shims
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/lib/Memory
)

# Inside the build tree, so two checkouts testing at once do not share a folder in /tmp.
target_compile_definitions(FolderWalkTest PRIVATE FOLDER_WALK_ROOT="${CMAKE_CURRENT_BINARY_DIR}/trees")

target_link_libraries(FolderWalkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FolderWalkTest)
