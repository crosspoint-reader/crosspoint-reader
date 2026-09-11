find_package(OpenSSL QUIET)
if(NOT OpenSSL_FOUND)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "KOReaderDocumentIdStackTest requires OpenSSL development files (Debian/Ubuntu: libssl-dev)")
  endif()
  message(STATUS "Skipping KOReaderDocumentIdStackTest: OpenSSL development files are unavailable")
  return()
endif()

option(KOREADER_DOCUMENT_ID_STACK_FORCE_NO_WRAP
  "Run document-ID API tests without allocation fault injection" OFF)
set(document_id_stack_wrap_supported FALSE)
if(NOT KOREADER_DOCUMENT_ID_STACK_FORCE_NO_WRAP
    AND CMAKE_SYSTEM_NAME STREQUAL "Linux"
    AND CMAKE_SIZEOF_VOID_P EQUAL 8
    AND NOT CMAKE_CROSSCOMPILING)
  include(CheckCXXSourceRuns)
  include(CMakePushCheckState)
  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_LINK_OPTIONS "-Wl,--wrap=_ZnamRKSt9nothrow_t")
  check_cxx_source_runs([=[
    #include <cstddef>
    #include <new>
    static_assert(sizeof(std::size_t) == 8);
    static bool intercepted = false;
    extern "C" void* __real__ZnamRKSt9nothrow_t(std::size_t, const std::nothrow_t&) noexcept;
    extern "C" void* __wrap__ZnamRKSt9nothrow_t(std::size_t size, const std::nothrow_t& tag) noexcept {
      intercepted = true;
      return __real__ZnamRKSt9nothrow_t(size, tag);
    }
    int main() {
      void* storage = ::operator new[](1024, std::nothrow);
      const bool supported = intercepted && storage;
      ::operator delete[](storage);
      return supported ? 0 : 1;
    }
  ]=] KOREADER_DOCUMENT_ID_STACK_CAN_WRAP_NOTHROW)
  cmake_pop_check_state()
  if(KOREADER_DOCUMENT_ID_STACK_CAN_WRAP_NOTHROW)
    set(document_id_stack_wrap_supported TRUE)
  endif()
endif()

add_executable(KOReaderDocumentIdStackTest
  KOReaderDocumentIdStackTest.cpp
  MD5BuilderTest.cpp
  ${REPO_ROOT}/lib/KOReaderSync/KOReaderDocumentId.cpp
)

target_include_directories(KOReaderDocumentIdStackTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/Memory
)

target_link_libraries(KOReaderDocumentIdStackTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  OpenSSL::Crypto
)

if(document_id_stack_wrap_supported)
  target_compile_definitions(KOReaderDocumentIdStackTest PRIVATE DOCUMENT_ID_STACK_WRAP_NOTHROW=1)
  target_link_options(KOReaderDocumentIdStackTest PRIVATE "-Wl,--wrap=_ZnamRKSt9nothrow_t")
  message(STATUS "Document-ID scratch allocation fault injection enabled")
else()
  target_compile_definitions(KOReaderDocumentIdStackTest PRIVATE DOCUMENT_ID_STACK_WRAP_NOTHROW=0)
  message(STATUS "Document-ID allocation wrapping unavailable or disabled: running API/digest tests; skipping fault injection and allocation counts")
endif()

gtest_discover_tests(KOReaderDocumentIdStackTest)
