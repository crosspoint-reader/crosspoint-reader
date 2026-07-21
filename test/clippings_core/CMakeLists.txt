add_executable(ClippingsCoreTest
  ClippingsCoreTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClipTextBuilder.cpp
)

target_include_directories(ClippingsCoreTest PRIVATE
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingsCoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingsCoreTest)

add_executable(ClippingStorePersistenceTest
  ClippingStorePersistenceTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClippingStore.cpp
)

target_include_directories(ClippingStorePersistenceTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingStorePersistenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingStorePersistenceTest)

add_executable(ClippingPageToolsTest
  ClippingPageToolsTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
  ${REPO_ROOT}/src/clippings/ClippingPageTools.cpp
)

target_include_directories(ClippingPageToolsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/clippings
)

target_link_libraries(ClippingPageToolsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingPageToolsTest)

add_executable(SavedClippingsModelTest
  SavedClippingsModelTest.cpp
)

target_include_directories(SavedClippingsModelTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(SavedClippingsModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SavedClippingsModelTest)

add_executable(ClippingJumpValidationTest
  ClippingJumpValidationTest.cpp
  ${REPO_ROOT}/src/clippings/ClippingCodec.cpp
)

target_include_directories(ClippingJumpValidationTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(ClippingJumpValidationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(ClippingJumpValidationTest)
