#pragma once

#ifndef CROSSPOINT_FIRMWARE_FAMILY
#define CROSSPOINT_FIRMWARE_FAMILY "crosspoint"
#endif

#ifndef CROSSPOINT_FIRMWARE_ID
#define CROSSPOINT_FIRMWARE_ID "crosspoint"
#endif

#ifndef CROSSPOINT_PLUGIN_API_VERSION
#define CROSSPOINT_PLUGIN_API_VERSION 1
#endif

#ifndef CROSSPOINT_PLUGIN_CAPABILITIES
#define CROSSPOINT_PLUGIN_CAPABILITIES \
  "catalog.lists,catalog.notices,catalog.bundle_install,catalog.version_tracking,catalog.compatibility"
#endif

namespace firmwareidentity {
inline constexpr const char* FAMILY = CROSSPOINT_FIRMWARE_FAMILY;
inline constexpr const char* ID = CROSSPOINT_FIRMWARE_ID;
inline constexpr int PLUGIN_API_VERSION = CROSSPOINT_PLUGIN_API_VERSION;
inline constexpr const char* PLUGIN_CAPABILITIES = CROSSPOINT_PLUGIN_CAPABILITIES;
}  // namespace firmwareidentity
