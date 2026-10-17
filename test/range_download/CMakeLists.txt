# The download loop, and where SecureHttpClient sends credentials, over a scripted network. The
# stubs (Arduino, Client, WiFiClient, WiFi, base64, Logging) come first on the include path; the stub
# SecureClient is in RangeDownloadTest.cpp.
add_executable(RangeDownloadTest
  RangeDownloadTest.cpp
  CredentialRedirectTest.cpp
  ${REPO_ROOT}/lib/SecureNet/src/RangeDownload.cpp
  ${REPO_ROOT}/lib/SecureNet/src/SecureHttpClient.cpp
  ${REPO_ROOT}/lib/SecureNet/src/HttpRange.cpp
)

target_include_directories(RangeDownloadTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/SecureNet/include
)

target_link_libraries(RangeDownloadTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(RangeDownloadTest)
