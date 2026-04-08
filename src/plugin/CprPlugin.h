#pragma once

/**
 * CprPlugin — Compile-time plugin descriptor for CrossPoint Reader.
 *
 * Each plugin fills in a static CprPlugin struct and adds it to the
 * pluginTable[] array in PluginRegistry.cpp.  See PLUGINS.md for the
 * full registration process and coding conventions.
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
  void (*onBookOpen)(const char* bookPath);
  void (*onBookClose)();
  void (*onPageTurn)(int chapter, int page);
  void (*onSleep)();
  void (*onWake)();
  /// Called when the plugin is enabled (via setEnabled or dispatchBoot for
  /// initially-enabled plugins).  Use this to start background tasks / queues.
  ///
  /// **Idempotency requirement**: onEnable() may be invoked more than once
  /// without an intervening onDisable() — for example, dispatchBoot() calls
  /// onEnable() for every persisted-enabled plugin, and a later setEnabled(true)
  /// can call it again.  Implementations MUST guard against repeated
  /// initialization (e.g. check `syncQueue != nullptr` before creating
  /// resources, or use an early-return flag) to avoid duplicate task/queue
  /// creation.
  void (*onEnable)();
  /// Called when the plugin is disabled at runtime (via setEnabled).
  /// Use this to stop background tasks / queues and free resources.
  void (*onDisable)();
};
