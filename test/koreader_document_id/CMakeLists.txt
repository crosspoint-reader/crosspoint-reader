find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "KOReaderDocumentIdTest requires OpenSSL development files (Debian/Ubuntu: libssl-dev)")
  endif()
  message(STATUS "Skipping KOReaderDocumentIdTest: OpenSSL development files are unavailable")
  return()
endif()

add_executable(KOReaderDocumentIdTest
  KOReaderDocumentIdTest.cpp
  MD5BuilderTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderDocumentId.cpp
)

target_include_directories(KOReaderDocumentIdTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(KOReaderDocumentIdTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  OpenSSL::Crypto
)

gtest_discover_tests(KOReaderDocumentIdTest)
