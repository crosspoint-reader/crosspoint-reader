set(I18N_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/i18n")
file(GLOB I18N_TRANSLATIONS CONFIGURE_DEPENDS "${REPO_ROOT}/lib/I18n/translations/*.yaml")
add_custom_command(
  OUTPUT "${I18N_OUTPUT_DIR}/I18nKeys.h"
  BYPRODUCTS "${I18N_OUTPUT_DIR}/I18nStrings.h" "${I18N_OUTPUT_DIR}/I18nStrings.cpp"
  COMMAND ${CMAKE_COMMAND} -E make_directory "${I18N_OUTPUT_DIR}"
  COMMAND ${Python3_EXECUTABLE} "${REPO_ROOT}/scripts/gen_i18n.py"
    "${REPO_ROOT}/lib/I18n/translations" "${I18N_OUTPUT_DIR}"
  DEPENDS "${REPO_ROOT}/scripts/gen_i18n.py" ${I18N_TRANSLATIONS}
  WORKING_DIRECTORY "${REPO_ROOT}"
  VERBATIM)

add_executable(BookTextSettingsTest
  BookTextSettingsTest.cpp
  ${REPO_ROOT}/src/BookTextSettings.cpp
  ${REPO_ROOT}/src/ReaderTextSettings.cpp
  "${I18N_OUTPUT_DIR}/I18nKeys.h")
target_link_libraries(BookTextSettingsTest PRIVATE crosspoint_test_common GTest::gtest_main bleturner_arduinojson)
target_include_directories(BookTextSettingsTest PRIVATE
  stubs ../library_builder/stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Serialization
  "${I18N_OUTPUT_DIR}" ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/BlePageTurner/include
  ${REPO_ROOT}/freeink-sdk/libs/network/BleKeyboardHost/include)
gtest_discover_tests(BookTextSettingsTest)
