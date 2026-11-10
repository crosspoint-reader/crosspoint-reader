add_executable(BookTextSettingsTest
  BookTextSettingsTest.cpp
  ${REPO_ROOT}/src/BookTextSettings.cpp
  ${REPO_ROOT}/src/ReaderTextSettings.cpp)
target_link_libraries(BookTextSettingsTest PRIVATE crosspoint_test_common GTest::gtest_main bleturner_arduinojson)
target_include_directories(BookTextSettingsTest PRIVATE
  stubs ../library_builder/stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/BlePageTurner/include
  ${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include)
gtest_discover_tests(BookTextSettingsTest)
