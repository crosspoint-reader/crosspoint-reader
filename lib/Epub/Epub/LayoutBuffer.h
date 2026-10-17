#pragma once

#include <Logging.h>
#include <Memory.h>

#include <limits>

// Fallible storage for layout ownership and offsets. Growth preserves existing
// entries on failure; clear() releases entries but reuses the allocated slots.
template <typename T>
class LayoutBuffer {
  static_assert(std::is_nothrow_default_constructible_v<T> && std::is_nothrow_move_assignable_v<T>);
  static constexpr size_t MAX_SIZE = (std::numeric_limits<size_t>::max() - sizeof(size_t)) / sizeof(T);
  std::unique_ptr<T[]> data;
  size_t count = 0;
  size_t slots = 0;

 public:
  LayoutBuffer() = default;
  LayoutBuffer(const LayoutBuffer&) = delete;
  LayoutBuffer& operator=(const LayoutBuffer&) = delete;
  size_t size() const { return count; }
  size_t capacity() const { return slots; }
  bool empty() const { return count == 0; }
  T& operator[](size_t index) { return data[index]; }
  const T& operator[](size_t index) const { return data[index]; }
  T* begin() { return data.get(); }
  T* end() { return count ? data.get() + count : data.get(); }
  const T* begin() const { return data.get(); }
  const T* end() const { return count ? data.get() + count : data.get(); }

  [[nodiscard]] bool reserve(size_t capacity) {
    if (capacity <= slots) return true;
    if (capacity > MAX_SIZE) {
      LOG_ERR("EPB", "Layout buffer size overflow");
      return false;
    }
    auto replacement = makeUniqueNoThrow<T[]>(capacity);
    if (!replacement) {
      LOG_ERR("EPB", "OOM: layout buffer %u slots", static_cast<unsigned>(capacity));
      return false;
    }
    for (size_t i = 0; i < count; ++i) replacement[i] = std::move(data[i]);
    data = std::move(replacement);
    slots = capacity;
    return true;
  }

  [[nodiscard]] bool push_back(T value) {
    if (count == slots) {
      const size_t next = slots == 0 ? 4 : (slots > MAX_SIZE / 2 ? MAX_SIZE : slots * 2);
      if (count == MAX_SIZE || !reserve(next)) return false;
    }
    data[count++] = std::move(value);
    return true;
  }

  void clear() {
    for (size_t i = 0; i < count; ++i) data[i] = T{};
    count = 0;
  }
};
