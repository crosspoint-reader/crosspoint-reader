#pragma once

#include "CprPlugin.h"

/**
 * PluginRegistry — manages all compiled-in plugins.
 *
 * Uses a simple explicit array approach (no linker-section tricks) for maximum
 * portability across ESP32-C3 toolchains.
 *
 * All plugin descriptors live in a static array.  Enabled/disabled state is
 * persisted to /.crosspoint/plugins.json on the SD card.
 */
class PluginRegistry {
 public:
  static constexpr int MAX_PLUGINS = 16;

  /// Call once after system init (SD card, settings loaded) to load enabled
  /// state and run compatibility checks.
  static void init();

  /// Query / mutate enabled state
  static bool isEnabled(const char* id);
  static void setEnabled(const char* id, bool enabled);

  /// Persist enabled state to SD card
  static void saveState();

  /// Dispatch hooks to all enabled + compatible plugins
  static void dispatchBoot();
  static void dispatchBookOpen(const char* path);
  static void dispatchBookClose();
  static void dispatchPageTurn(int chapter, int page);
  static void dispatchSleep();
  static void dispatchWake();

  /// Accessors for the Settings UI
  static int count();
  static const CprPlugin* get(int index);
  static bool isCompatible(int index);

 private:
  PluginRegistry() = delete;  // all-static class
};
