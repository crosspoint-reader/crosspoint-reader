/**
 * HelloPlugin — Minimal example plugin for CrossPoint Reader.
 *
 * This file serves as the reference template for plugin authors.  Copy it,
 * rename it, fill in the CprPlugin struct with your own metadata and hook
 * implementations, and add your plugin descriptor to the pluginTable in
 * src/plugin/PluginRegistry.cpp.
 *
 * CONVENTIONS (see .skills/SKILL.md):
 *  - Use LOG_DBG / LOG_INF / LOG_ERR for all output; never raw Serial.
 *  - Keep stack locals under 256 bytes.
 *  - Avoid std::string in hot paths; use const char* or snprintf.
 *  - Mark large constant data `static constexpr` to keep it in Flash.
 *  - Plugin hooks must return quickly — they run on the main loop thread.
 *
 * This plugin is DISABLED by default.  Enable it from Settings → Plugins
 * to see its log output in the serial monitor.
 */

#include <Logging.h>

#include "plugin/CprPlugin.h"

// ---------------------------------------------------------------------------
// Hook implementations
// ---------------------------------------------------------------------------

/// Called once at boot when the plugin is enabled.
static void helloBoot() { LOG_INF("HELLO", "hello_plugin loaded"); }

/// Called on every page turn while a book is open.
static void helloPageTurn(int chapter, int page) { LOG_DBG("HELLO", "Page turn: chapter=%d page=%d", chapter, page); }

// ---------------------------------------------------------------------------
// Plugin descriptor
// ---------------------------------------------------------------------------

/// The plugin descriptor.  Every field is required; set unused hooks to
/// nullptr.  The `id` must be unique across all plugins and use snake_case.
///
/// `minCpr` is the minimum CrossPoint firmware version this plugin is
/// compatible with.  If the running firmware is older, the plugin will be
/// disabled automatically and the Settings UI will show a warning.
extern const CprPlugin helloPlugin = {
    .id = "hello_plugin",
    .name = "Hello Plugin",
    .version = "1.0.0",
    .author = "CrossPoint",
    .minCpr = CROSSPOINT_VERSION,  // compatible with the version it was built with
    .description = "Minimal example plugin — logs boot and page turns",

    .onBoot = helloBoot,
    .onBookOpen = nullptr,
    .onBookClose = nullptr,
    .onPageTurn = helloPageTurn,
    .onSleep = nullptr,
    .onWake = nullptr,
};
