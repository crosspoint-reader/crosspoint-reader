#pragma once
#if (defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND) || defined(CROSSPOINT_LOCALSEND_HOST_TEST)
#include <ArduinoJson.h>

#include <cstddef>
#include <cstdlib>

namespace localsend {
// ArduinoJson owns allocations through this adapter and returns them to it.
class BoundedJsonAllocator final : public ArduinoJson::Allocator {
  struct alignas(std::max_align_t) Header {
    size_t bytes;
  };
  size_t used = 0;

 public:
  static constexpr size_t LIMIT = 16384;
  void* allocate(size_t size) override {
    if (size > LIMIT - used || sizeof(Header) > LIMIT - used - size) return nullptr;
    auto* header = static_cast<Header*>(std::malloc(sizeof(Header) + size));
    if (!header) return nullptr;
    header->bytes = size + sizeof(Header);
    used += header->bytes;
    return header + 1;
  }
  void deallocate(void* pointer) override {
    if (!pointer) return;
    auto* header = static_cast<Header*>(pointer) - 1;
    used -= header->bytes;
    std::free(header);
  }
  void* reallocate(void* pointer, size_t size) override {
    if (!pointer) return allocate(size);
    if (!size) {
      deallocate(pointer);
      return nullptr;
    }
    auto* old = static_cast<Header*>(pointer) - 1;
    const size_t remaining = LIMIT - (used - old->bytes);
    if (size > remaining || sizeof(Header) > remaining - size) return nullptr;
    const size_t oldBytes = old->bytes;
    auto* header = static_cast<Header*>(std::realloc(old, size + sizeof(Header)));
    if (!header) return nullptr;
    header->bytes = size + sizeof(Header);
    used = used - oldBytes + header->bytes;
    return header + 1;
  }
};
}  // namespace localsend
#endif
