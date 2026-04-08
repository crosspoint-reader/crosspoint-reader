#pragma once

/**
 * CprPlugin — Compile-time plugin descriptor for CrossPoint Reader.
 *
 * Each plugin fills in a static CprPlugin struct and registers it with
 * CPR_PLUGIN_REGISTER().  The plugin registry collects all registered
 * descriptors at startup and manages enable/disable state plus hook dispatch.
 *
 * All hook function pointers are optional — set unused ones to nullptr.
 */
struct CprPlugin {
  const char* id;           // short snake_case identifier, e.g. "reading_stats"
  const char* name;         // human-readable, shown in Settings
  const char* version;      // plugin version string
  const char* author;       // shown in Settings detail
  const char* minCpr;       // minimum CrossPoint version required, e.g. "1.2.0"
  const char* description;  // one-line description shown in Settings

  // Lifecycle hooks — all optional, set unused ones to nullptr
  void (*onBoot)();
  void (*onSettingsRender)();
  void (*onBookOpen)(const char* epubPath);
  void (*onBookClose)();
  void (*onPageTurn)(int chapter, int page);
  void (*onSleep)();
  void (*onWake)();
};
