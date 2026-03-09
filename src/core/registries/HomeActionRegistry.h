#pragma once

#include <Logging.h>

#include <cstddef>
#include <cstring>

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace core {

struct HomeActionEntry {
  const char* actionId;  // unique identifier e.g. anki, todo_planner

  struct HomeActionContext {
    bool hasOpdsUrl;
  };

  // Runtime capability and context check. Returns false when the action should stay hidden.
  bool (*shouldExpose)(HomeActionContext ctx);

  Activity* (*create)(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                      void (*onBack)(void* ctx));
};

class HomeActionRegistry {
 public:
  static constexpr int kMaxEntries = 16;

  static void add(const HomeActionEntry& entry) {
    // cppcheck-suppress knownConditionTrueFalse
    if (count >= kMaxEntries) {
      LOG_ERR("REG", "HomeActionRegistry full (%d), entry dropped", kMaxEntries);
      return;
    }

    entries[count++] = entry;
  }

  // Returns the total number of exposed entries. When outEntries is non-null, fills up to maxOut pointers.
  static int getExposed(HomeActionEntry::HomeActionContext ctx, const HomeActionEntry** outEntries, int maxOut) {
    const int safeMaxOut = maxOut > 0 ? maxOut : 0;
    int exposedCount = 0;
    int outCount = 0;

    for (int i = 0; i < count; ++i) {
      if (entries[i].shouldExpose == nullptr || !entries[i].shouldExpose(ctx)) {
        continue;
      }

      if (outEntries != nullptr && outCount < safeMaxOut) {
        outEntries[outCount++] = &entries[i];
      }

      ++exposedCount;
    }

    return exposedCount;
  }

  static bool shouldExpose(const char* actionId, HomeActionEntry::HomeActionContext ctx) {
    const HomeActionEntry* entry = find(actionId);
    return entry != nullptr && entry->shouldExpose != nullptr && entry->shouldExpose(ctx);
  }

  static Activity* create(const char* actionId, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          HomeActionEntry::HomeActionContext ctx, void* callbackCtx, void (*onBack)(void* ctx)) {
    const HomeActionEntry* entry = find(actionId);
    if (entry == nullptr || entry->shouldExpose == nullptr || !entry->shouldExpose(ctx) || entry->create == nullptr) {
      return nullptr;
    }
    return entry->create(renderer, mappedInput, callbackCtx, onBack);
  }

 private:
  static const HomeActionEntry* find(const char* actionId) {
    if (actionId == nullptr) {
      return nullptr;
    }

    for (int i = 0; i < count; ++i) {
      if (cStringsEqual(entries[i].actionId, actionId)) {
        return &entries[i];
      }
    }

    return nullptr;
  }

  static bool cStringsEqual(const char* left, const char* right) {
    if (left == nullptr || right == nullptr) {
      return left == right;
    }
    return std::strcmp(left, right) == 0;
  }

  inline static HomeActionEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
