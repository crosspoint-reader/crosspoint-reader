#pragma once

#include <Logging.h>

#include <cstddef>

#include "activities/settings/SettingsActivity.h"

class Activity;
class GfxRenderer;
class MappedInputManager;

namespace core {

struct SettingsActionEntry {
  SettingAction action;
  bool (*isSupported)();
  Activity* (*create)(GfxRenderer& renderer, MappedInputManager& mappedInput, void* callbackCtx,
                      void (*onComplete)(void* ctx), void (*onCompleteBool)(void* ctx, bool result));
};

class SettingsActionRegistry {
 public:
  static constexpr int kMaxEntries = 24;

  static void add(const SettingsActionEntry& entry) {
    // cppcheck-suppress knownConditionTrueFalse
    if (count >= kMaxEntries) {
      LOG_ERR("REG", "SettingsActionRegistry full (%d), entry dropped", kMaxEntries);
      return;
    }

    entries[count++] = entry;
  }

  static bool isSupported(SettingAction action) {
    const SettingsActionEntry* entry = find(action);
    return entry != nullptr && entry->isSupported != nullptr && entry->isSupported();
  }

  static Activity* create(SettingAction action, GfxRenderer& renderer, MappedInputManager& mappedInput,
                          void* callbackCtx, void (*onComplete)(void* ctx),
                          void (*onCompleteBool)(void* ctx, bool result)) {
    const SettingsActionEntry* entry = find(action);
    if (entry == nullptr || entry->isSupported == nullptr || !entry->isSupported() || entry->create == nullptr) {
      return nullptr;
    }

    return entry->create(renderer, mappedInput, callbackCtx, onComplete, onCompleteBool);
  }

 private:
  static const SettingsActionEntry* find(SettingAction action) {
    for (int i = 0; i < count; ++i) {
      if (entries[i].action == action) {
        return &entries[i];
      }
    }

    return nullptr;
  }

  inline static SettingsActionEntry entries[kMaxEntries] = {};
  inline static int count = 0;
};

}  // namespace core
