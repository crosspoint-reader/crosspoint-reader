#pragma once

// Plugin-system contract advertised to catalogs and the web UI.
#if defined(CROSSPOINT_LOCALSEND) && CROSSPOINT_LOCALSEND
#define CROSSPOINT_LOCALSEND_CAPABILITY ",transfer.localsend.v1"
#else
#define CROSSPOINT_LOCALSEND_CAPABILITY ""
#endif
namespace firmwareidentity {
inline constexpr const char* FAMILY = "crosspoint";
inline constexpr int PLUGIN_API_VERSION = 1;
inline constexpr const char* PLUGIN_CAPABILITIES =
    "catalog.lists,catalog.notices,catalog.bundle_install,catalog.version_tracking,catalog.compatibility,"
    "catalog.item_details,catalog.encoded_item_fields,catalog.list_index_mapping,catalog.list_requirements,"
    "catalog.server_paths" CROSSPOINT_LOCALSEND_CAPABILITY;
}  // namespace firmwareidentity
