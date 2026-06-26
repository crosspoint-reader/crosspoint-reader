#include <cstdlib>

extern "C" void* __real_malloc(std::size_t size) { return std::malloc(size); }

extern "C" void* __real_calloc(std::size_t nmemb, std::size_t size) { return std::calloc(nmemb, size); }

extern "C" void* __real_realloc(void* ptr, std::size_t size) { return std::realloc(ptr, size); }

extern "C" void __real_free(void* ptr) { std::free(ptr); }
