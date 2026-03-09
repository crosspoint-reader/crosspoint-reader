#pragma once

#include <cstddef>
#include <string>

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace core {

struct ReaderOpenResult {
  enum class Status { Opened, Unsupported, LoadFailed };
  Status status = Status::LoadFailed;
  Activity* activity = nullptr;
  const char* logMessage = nullptr;
  const char* uiMessage = nullptr;
};

struct ReaderEntry {
  const char* extension;                  // file extension e.g. .epub
  bool (*isSupported)(const char* path);  // runtime capability check (returns false if feature disabled)
  const char* unsupportedLogMessage;
  const char* unsupportedUiMessage;
  // Factory: allocates and returns a new Activity*. Caller owns it. Returns nullptr on load failure.
  Activity* (*create)(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& path,
                      void* callbackCtx, void (*onBackToLibrary)(void* ctx, const std::string& path),
                      void (*onBackHome)(void* ctx));
};

class ReaderRegistry {
 public:
  static constexpr int kMaxEntries = 16;

  static void add(const ReaderEntry& entry) {
    if (count >= kMaxEntries) {
      return;
    }

    entries[count++] = entry;
  }

  static const ReaderEntry* find(const std::string& path) {
    for (int i = 0; i < count; ++i) {
      if (hasExtension(path, entries[i].extension)) {
        return &entries[i];
      }
    }

    return nullptr;
  }

  static bool isSupportedLibraryFile(const std::string& path) {
    const ReaderEntry* entry = find(path);
    return entry != nullptr && entry->isSupported != nullptr && entry->isSupported(path.c_str());
  }

  // Open a reader for path. callbackCtx/onBackToLibrary/onBackHome are forwarded verbatim to
  // the registered factory; pass nullptr if the callbacks are not needed.
  static ReaderOpenResult open(const std::string& path, GfxRenderer& renderer, MappedInputManager& mappedInput,
                               void* callbackCtx, void (*onBackToLibrary)(void* ctx, const std::string& bookPath),
                               void (*onBackHome)(void* ctx)) {
    if (path.empty()) {
      return {};
    }
    const ReaderEntry* entry = find(path);
    if (entry == nullptr) {
      return {ReaderOpenResult::Status::Unsupported, nullptr, "Unsupported format", "Unsupported\nformat"};
    }
    if (entry->isSupported != nullptr && !entry->isSupported(path.c_str())) {
      return {ReaderOpenResult::Status::Unsupported, nullptr, entry->unsupportedLogMessage,
              entry->unsupportedUiMessage};
    }
    if (entry->create == nullptr) {
      return {};
    }
    Activity* activity = entry->create(renderer, mappedInput, path, callbackCtx, onBackToLibrary, onBackHome);
    if (activity == nullptr) {
      return {};
    }
    return {ReaderOpenResult::Status::Opened, activity, nullptr, nullptr};
  }

 private:
  static std::size_t cStringLength(const char* value) {
    if (value == nullptr) {
      return 0;
    }

    std::size_t length = 0;
    while (value[length] != '\0') {
      ++length;
    }
    return length;
  }

  static bool hasExtension(const std::string& path, const char* extension) {
    const std::size_t extensionLength = cStringLength(extension);
    if (extensionLength == 0 || path.size() < extensionLength) {
      return false;
    }

    const std::size_t offset = path.size() - extensionLength;
    for (std::size_t i = 0; i < extensionLength; ++i) {
      if (path[offset + i] != extension[i]) {
        return false;
      }
    }

    return true;
  }

  inline static ReaderEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
