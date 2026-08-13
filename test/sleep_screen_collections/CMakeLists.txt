add_executable(SleepScreenCollectionTest
  SleepScreenCollectionTest.cpp
  ${REPO_ROOT}/src/util/SleepScreenCollection.cpp
)

target_include_directories(SleepScreenCollectionTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(SleepScreenCollectionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SleepScreenCollectionTest)
