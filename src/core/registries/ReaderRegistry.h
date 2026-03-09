#pragma once

#include <Logging.h>

#include <cctype>
#include <cstddef>
#include <cstring>
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
    // cppcheck-suppress knownConditionTrueFalse
    if (count >= kMaxEntries) {
      LOG_ERR("REG", "ReaderRegistry full (%d), entry dropped", kMaxEntries);
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
  static bool hasExtension(const std::string& path, const char* extension) {
    if (extension == nullptr) {
      return false;
    }
    const std::size_t extensionLength = std::strlen(extension);
    if (extensionLength == 0 || path.size() < extensionLength) {
      return false;
    }

    const std::size_t offset = path.size() - extensionLength;
    for (std::size_t i = 0; i < extensionLength; ++i) {
      if (std::tolower(static_cast<unsigned char>(path[offset + i])) !=
          std::tolower(static_cast<unsigned char>(extension[i]))) {
        return false;
      }
    }

    return true;
  }

  inline static ReaderEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
