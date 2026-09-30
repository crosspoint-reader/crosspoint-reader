add_executable(CredentialIntegrityTest
  CredentialIntegrityTest.cpp
)

target_link_libraries(CredentialIntegrityTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(CredentialIntegrityTest)
