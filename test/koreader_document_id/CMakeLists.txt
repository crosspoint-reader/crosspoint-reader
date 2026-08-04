add_executable(OriginalDocumentIdMetadataTest
  OriginalDocumentIdMetadataTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/OriginalDocumentIdMetadata.cpp
)

target_include_directories(OriginalDocumentIdMetadataTest PRIVATE
  ${REPO_ROOT}/lib/Epub/Epub/parsers
)

target_link_libraries(OriginalDocumentIdMetadataTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OriginalDocumentIdMetadataTest)
