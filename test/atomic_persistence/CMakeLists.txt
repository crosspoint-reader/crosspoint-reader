add_executable(AtomicPersistenceTest
  AtomicPersistenceTest.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/Serialization/StagedFileTransaction.cpp
  ${REPO_ROOT}/src/LegacyStateCodec.cpp
  ${REPO_ROOT}/lib/hal/ClockCalendar.cpp
)

target_include_directories(AtomicPersistenceTest PRIVATE
  ${REPO_ROOT}/test/epub_source_identity/stubs
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/hal
)

target_link_libraries(AtomicPersistenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AtomicPersistenceTest)
