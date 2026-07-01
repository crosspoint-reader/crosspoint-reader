#pragma once

#include <functional>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * Memory-efficient open-addressed flat hash map for constrained systems.
 * Uses parallel vectors (struct-of-arrays) so the occupied[] bitfield is
 * contiguous and scanned without touching key/value memory. Robin Hood hashing
 * keeps miss probes shorter even when callers reserve an exact slot count.
 * No erase() — clear() resets everything. No tombstones needed.
 */
template <typename Key, typename Value, typename Hash = std::hash<Key>, typename Equal = std::equal_to<Key>>
class FlatMap {
 public:
  FlatMap() = default;

  void clear() {
    std::vector<Key>().swap(keys_);
    std::vector<Value>().swap(values_);
    std::vector<bool>().swap(occupied_);
    size_ = 0;
  }

  void reserve(size_t capacity) {
    if (capacity <= keys_.size()) return;
    if (keys_.empty()) {
      keys_.resize(capacity);
      values_.resize(capacity);
      occupied_.assign(capacity, false);
    } else {
      rehash(capacity);
    }
  }

  [[nodiscard]] size_t size() const { return size_; }
  [[nodiscard]] bool empty() const { return size_ == 0; }

  template <bool IsConst>
  struct MapIterator {
    using FlatMapType = std::conditional_t<IsConst, const FlatMap, FlatMap>;

    FlatMapType* map;
    size_t index;

    MapIterator& operator++() {
      do {
        ++index;
      } while (index < map->keys_.size() && !map->occupied_[index]);
      return *this;
    }

    bool operator==(const MapIterator& other) const { return index == other.index; }
    bool operator!=(const MapIterator& other) const { return index != other.index; }

    // Expose first/second like std::pair via a proxy
    struct Proxy {
      std::conditional_t<IsConst, const Key, Key>& first;
      std::conditional_t<IsConst, const Value, Value>& second;
    };
    Proxy operator*() const { return {map->keys_[index], map->values_[index]}; }

    struct ArrowProxy {
      Proxy p;
      Proxy* operator->() { return &p; }
    };
    ArrowProxy operator->() const { return {{map->keys_[index], map->values_[index]}}; }

    template <bool OtherConst>
      requires(IsConst && !OtherConst)
    explicit MapIterator(const MapIterator<OtherConst>& other) : map(other.map), index(other.index) {}

    explicit MapIterator(FlatMapType* m, size_t idx) : map(m), index(idx) {}
  };

  using Iterator = MapIterator<false>;
  using ConstIterator = MapIterator<true>;

  Iterator begin() {
    size_t i = 0;
    while (i < keys_.size() && !occupied_[i]) ++i;
    return Iterator{this, i};
  }
  Iterator end() { return Iterator{this, keys_.size()}; }

  ConstIterator begin() const {
    size_t i = 0;
    while (i < keys_.size() && !occupied_[i]) ++i;
    return ConstIterator{this, i};
  }
  ConstIterator end() const { return ConstIterator{this, keys_.size()}; }

  template <typename K>
  Iterator find(const K& key) {
    if (keys_.empty()) return end();
    size_t idx = findSlot(key);
    if (idx < keys_.size() && occupied_[idx]) return Iterator{this, idx};
    return end();
  }

  template <typename K>
  ConstIterator find(const K& key) const {
    if (keys_.empty()) return end();
    size_t idx = findSlot(key);
    if (idx < keys_.size() && occupied_[idx]) return ConstIterator{this, idx};
    return end();
  }

  Value& operator[](const Key& key) {
    if (!keys_.empty()) {
      size_t idx = findSlot(key);
      if (idx < keys_.size() && occupied_[idx]) return values_[idx];
    }
    if (keys_.empty() || size_ == keys_.size()) grow();
    size_t idx = insertHelper(key, Value{});
    return values_[idx];
  }

  void emplace(Key key, Value value) {
    if (keys_.empty() || size_ == keys_.size()) grow();
    insertHelper(std::move(key), std::move(value));
  }

 private:
  // Parallel vectors: occupied_ is scanned hot, keys_/values_ only touched on hit.
  std::vector<Key> keys_;
  std::vector<Value> values_;
  std::vector<bool> occupied_;
  size_t size_ = 0;

  void grow() {
    size_t cap = keys_.size();
    size_t newCapacity = cap == 0 ? 16 : cap > 128 ? cap + cap / 2 : cap * 2;
    rehash(newCapacity);
  }

  static size_t probeDistance(size_t idealIdx, size_t idx, size_t cap) {
    return idx >= idealIdx ? idx - idealIdx : idx + cap - idealIdx;
  }

  template <typename K>
  size_t idealIndex(const K& key, size_t cap) const {
    return Hash{}(key) % cap;
  }

  void rehash(size_t newCapacity) {
    std::vector<Key> oldKeys = std::move(keys_);
    std::vector<Value> oldValues = std::move(values_);
    std::vector<bool> oldOccupied = std::move(occupied_);

    keys_.resize(newCapacity);
    values_.resize(newCapacity);
    occupied_.assign(newCapacity, false);
    size_ = 0;

    for (size_t i = 0; i < oldKeys.size(); ++i) {
      if (oldOccupied[i]) {
        insertHelper(std::move(oldKeys[i]), std::move(oldValues[i]));
      }
    }
  }

  template <typename K>
  size_t findSlot(const K& key) const {
    size_t cap = keys_.size();
    size_t idx = idealIndex(key, cap);
    size_t dist = 0;
    for (size_t probe = 0; probe < cap; ++probe) {
      if (!occupied_[idx]) return cap;  // not found — return sentinel
      if (Equal{}(keys_[idx], key)) return idx;
      if (probeDistance(idealIndex(keys_[idx], cap), idx, cap) < dist) {
        return cap;  // Robin Hood early-exit: key would have been swapped forward already
      }
      if (++idx >= cap) idx = 0;
      ++dist;
    }
    return cap;
  }

  size_t insertHelper(Key key, Value value) {
    size_t cap = keys_.size();
    size_t idx = idealIndex(key, cap);
    size_t dist = 0;
    for (size_t probe = 0; probe < cap; ++probe) {
      if (!occupied_[idx]) {
        keys_[idx] = std::move(key);
        values_[idx] = std::move(value);
        occupied_[idx] = true;
        ++size_;
        return idx;
      }
      if (Equal{}(keys_[idx], key)) {
        values_[idx] = std::move(value);
        return idx;
      }
      size_t occupantDist = probeDistance(idealIndex(keys_[idx], cap), idx, cap);
      if (occupantDist < dist) {
        std::swap(key, keys_[idx]);
        std::swap(value, values_[idx]);
        dist = occupantDist;
      }
      if (++idx >= cap) idx = 0;
      ++dist;
    }
    return cap;  // unreachable if size_ < cap
  }
};
