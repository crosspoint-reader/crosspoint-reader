add_executable(KOReaderDocumentIdTest
  KOReaderDocumentIdTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderDocumentId.cpp
  ${REPO_ROOT}/test/zip_entry_reader/LoggingStub.cpp
)

target_include_directories(KOReaderDocumentIdTest PRIVATE
  # In-memory HalStorage.h and a recording MD5Builder.h; shadow the test/shims HalStorage.h stub.
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Logging
  ${REPO_ROOT}/test/shims           # Print.h, HardwareSerial.h, WString.h
)

target_link_libraries(KOReaderDocumentIdTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(KOReaderDocumentIdTest)
