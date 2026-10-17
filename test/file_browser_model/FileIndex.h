#pragma once

// Host fake for lib/FileIndex/FileIndex.h: the interface FileBrowserModel uses, over the stdio
// storage shim. The real one writes its index under /.crosspoint on the card, which a host test
// cannot give it; this one answers from the directory itself, keeping nothing per entry, so what
// the model holds on the heap is all the test measures. Rows are the shim's enumeration order.
//
// `failOpen` makes open() fail, as the real one does on an SD write error: the model's in-RAM
// fallback.

#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

class FileIndex {
 public:
  enum class SortMode : uint8_t { Name = 0, Date = 1, Size = 2, Type = 3 };
  static constexpr size_t MAX_NAME = 511;

  struct Entry {
    uint32_t size;
    uint32_t dateTime;
    bool isDir;
    char name[MAX_NAME + 1];
  };

  using AcceptFn = bool (*)(const char* name, bool isDir);

  static inline bool failOpen = false;

  FileIndex() = default;
  ~FileIndex() { close(); }
  FileIndex(const FileIndex&) = delete;
  FileIndex& operator=(const FileIndex&) = delete;

  bool open(const char* dirPath, SortMode, AcceptFn acceptFn) {
    close();
    if (failOpen) return false;
    path = dirPath;
    accept = acceptFn;
    count = 0;
    auto dir = Storage.open(dirPath);
    if (!dir || !dir.isDirectory()) return false;
    char name[MAX_NAME + 1];
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(name, sizeof(name));
      if (accept(name, file.isDirectory())) ++count;
    }
    opened = true;
    return true;
  }
  void close() { opened = false; }
  bool isOpen() const { return opened; }

  size_t totalCount() const { return count; }

  bool entryAt(const size_t row, bool /*descending*/, Entry& out) { return opened && row < count && nth(row, out); }

  size_t findRowByName(const char* name, bool /*descending*/) {
    Entry entry{};
    for (size_t row = 0; row < count && nth(row, entry); ++row)
      if (strcmp(entry.name, name) == 0) return row;
    return SIZE_MAX;
  }

 private:
  // The `row`th entry `accept` lets through, read from the directory again.
  bool nth(const size_t row, Entry& out) const {
    auto dir = Storage.open(path.c_str());
    if (!dir || !dir.isDirectory()) return false;
    size_t seen = 0;
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      file.getName(out.name, sizeof(out.name));
      out.isDir = file.isDirectory();
      if (!accept(out.name, out.isDir)) continue;
      if (seen++ < row) continue;
      out.size = out.isDir ? 0 : static_cast<uint32_t>(file.fileSize());
      out.dateTime = 0;
      return true;
    }
    return false;
  }

  std::string path;
  AcceptFn accept = nullptr;
  size_t count = 0;
  bool opened = false;
};
