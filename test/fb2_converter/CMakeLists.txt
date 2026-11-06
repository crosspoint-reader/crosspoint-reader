add_executable(Fb2ConverterTest
  Base64DecoderTest.cpp
  Fb2EncodingTest.cpp
  Fb2MetadataTest.cpp
  Fb2StructureTest.cpp
)

target_include_directories(Fb2ConverterTest PRIVATE
  ${REPO_ROOT}/lib/Fb2/src
  ${REPO_ROOT}/lib/expat
)

target_link_libraries(Fb2ConverterTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(Fb2ConverterTest)
