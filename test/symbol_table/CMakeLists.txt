find_package(Python3 REQUIRED COMPONENTS Interpreter)

# The images come from the firmware's own generator, so the test fails when
# the generator and the reader stop agreeing on the table format.
set(SYMBOL_TABLE_FIXTURE_DIR ${CMAKE_CURRENT_BINARY_DIR}/fixtures)
add_custom_command(
  OUTPUT ${SYMBOL_TABLE_FIXTURE_DIR}/with_table.bin ${SYMBOL_TABLE_FIXTURE_DIR}/without_table.bin
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/make_fixture.py
          ${REPO_ROOT}/scripts/gen_symbol_table.py ${SYMBOL_TABLE_FIXTURE_DIR}
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/make_fixture.py ${REPO_ROOT}/scripts/gen_symbol_table.py
  VERBATIM
)
add_custom_target(SymbolTableFixtures
  DEPENDS ${SYMBOL_TABLE_FIXTURE_DIR}/with_table.bin ${SYMBOL_TABLE_FIXTURE_DIR}/without_table.bin)

add_executable(SymbolTableTest
  SymbolTableTest.cpp
  ${REPO_ROOT}/lib/hal/SymbolTable.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)
add_dependencies(SymbolTableTest SymbolTableFixtures)

target_include_directories(SymbolTableTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/hal
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(SymbolTableTest PRIVATE SYMBOL_TABLE_FIXTURE_DIR="${SYMBOL_TABLE_FIXTURE_DIR}")

target_link_libraries(SymbolTableTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SymbolTableTest)
