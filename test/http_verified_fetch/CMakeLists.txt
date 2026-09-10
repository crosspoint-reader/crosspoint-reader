# The same suite is built twice, once per TLS backend: kVerifiedFetchNeedsAnchors
# -- and so whether a missing CA bundle is fatal -- is decided at compile time by
# FREEINK_NET_WOLFSSL.
foreach(backend crt_bundle wolfssl)
  set(target HttpVerifiedFetchTest_${backend})
  add_executable(${target}
    HttpVerifiedFetchTest.cpp
  )

  target_include_directories(${target} PRIVATE
    ${REPO_ROOT}/src/network
  )

  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  gtest_discover_tests(${target} TEST_PREFIX ${backend}.)
endforeach()

target_compile_definitions(HttpVerifiedFetchTest_wolfssl PRIVATE FREEINK_NET_WOLFSSL=1)
