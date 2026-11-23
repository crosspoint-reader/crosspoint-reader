add_executable(AbsoluteGrayscaleTest AbsoluteGrayscaleTest.cpp ImagePackedBandTest.cpp
  ${REPO_ROOT}/lib/ImageOps/src/TilePlanes.cpp)
target_include_directories(AbsoluteGrayscaleTest PRIVATE ${REPO_ROOT}/lib/ImageOps/include)
target_compile_definitions(AbsoluteGrayscaleTest PRIVATE CROSSPOINT_ACCELERATED_IMAGES=1)
target_link_libraries(AbsoluteGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AbsoluteGrayscaleTest)
