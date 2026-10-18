find_package(EXPAT REQUIRED)
set(OPDS_ROOT ${REPO_ROOT}/freeink-sdk/libs/network/Opds)
file(GLOB OPDS_SOURCES CONFIGURE_DEPENDS ${OPDS_ROOT}/src/*.cpp)
add_executable(OpdsCredentialsTest OpdsCredentialsTest.cpp ${OPDS_SOURCES}
  ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp)
target_include_directories(OpdsCredentialsTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${OPDS_ROOT}/include ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/include
  ${REPO_ROOT}/freeink-sdk/libs/network/SecureNet/test/host/stubs)
target_compile_definitions(OpdsCredentialsTest PRIVATE FREEINK_NET_WOLFSSL CROSSPOINT_VERSION="test")
target_link_libraries(OpdsCredentialsTest PRIVATE crosspoint_test_common EXPAT::EXPAT GTest::gtest_main)
gtest_discover_tests(OpdsCredentialsTest)
