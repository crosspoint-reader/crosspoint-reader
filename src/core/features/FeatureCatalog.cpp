#include "core/features/FeatureCatalog.h"

#include <FeatureFlags.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace core {
namespace {

constexpr const char* kRequiresBookImagesAny[] = {"epub_support", "markdown"};
constexpr const char* kRequiresKOReaderSyncAll[] = {"integrations"};
constexpr const char* kRequiresCalibreSyncAll[] = {"integrations"};
constexpr const char* kRequiresWebPokedexPluginAll[] = {"image_sleep"};
constexpr const char* kRequiresPokemonPartyAll[] = {"web_pokedex_plugin"};
constexpr const char* kRequiresHyphenationAll[] = {"epub_support"};
constexpr const char* kRequiresLyraThemeAll[] = {"home_media_picker"};
constexpr const char* kRequiresBleWifiProvisioningAll[] = {"web_wifi_setup"};
constexpr const char* kRequiresOpenDyslexicFontsAny[] = {"bookerly_fonts", "notosans_fonts"};

constexpr FeatureDescriptor kFeatureCatalog[] = {
    {"extended_fonts", "Extended Fonts", ENABLE_EXTENDED_FONTS != 0, nullptr, 0, nullptr, 0},
    {"bookerly_fonts", "Bookerly Fonts", ENABLE_BOOKERLY_FONTS != 0, nullptr, 0, nullptr, 0},
    {"notosans_fonts", "NotoSans Fonts", ENABLE_NOTOSANS_FONTS != 0, nullptr, 0, nullptr, 0},
    {"opendyslexic_fonts", "OpenDyslexic", ENABLE_OPENDYSLEXIC_FONTS != 0, nullptr, 0, kRequiresOpenDyslexicFontsAny,
     sizeof(kRequiresOpenDyslexicFontsAny) / sizeof(kRequiresOpenDyslexicFontsAny[0])},
    {"image_sleep", "Image Sleep", ENABLE_IMAGE_SLEEP != 0, nullptr, 0, nullptr, 0},
    {"book_images", "Book Images", ENABLE_BOOK_IMAGES != 0, nullptr, 0, kRequiresBookImagesAny,
     sizeof(kRequiresBookImagesAny) / sizeof(kRequiresBookImagesAny[0])},
    {"markdown", "Markdown", ENABLE_MARKDOWN != 0, nullptr, 0, nullptr, 0},
    {"integrations", "Integrations", ENABLE_INTEGRATIONS != 0, nullptr, 0, nullptr, 0},
    {"koreader_sync", "KOReader Sync", ENABLE_KOREADER_SYNC != 0, kRequiresKOReaderSyncAll,
     sizeof(kRequiresKOReaderSyncAll) / sizeof(kRequiresKOReaderSyncAll[0]), nullptr, 0},
    {"calibre_sync", "Calibre Sync", ENABLE_CALIBRE_SYNC != 0, kRequiresCalibreSyncAll,
     sizeof(kRequiresCalibreSyncAll) / sizeof(kRequiresCalibreSyncAll[0]), nullptr, 0},
    {"background_server", "Background Server", ENABLE_BACKGROUND_SERVER != 0, nullptr, 0, nullptr, 0},
    {"background_server_on_charge", "Background Server On Charge", ENABLE_BACKGROUND_SERVER_ON_CHARGE != 0, nullptr, 0,
     nullptr, 0},
    {"background_server_always", "Background Server Always", ENABLE_BACKGROUND_SERVER_ALWAYS != 0, nullptr, 0, nullptr,
     0},
    {"home_media_picker", "Home Media Picker", ENABLE_HOME_MEDIA_PICKER != 0, nullptr, 0, nullptr, 0},
    {"web_pokedex_plugin", "Web Pokedex", ENABLE_WEB_POKEDEX_PLUGIN != 0, kRequiresWebPokedexPluginAll,
     sizeof(kRequiresWebPokedexPluginAll) / sizeof(kRequiresWebPokedexPluginAll[0]), nullptr, 0},
    {"pokemon_party", "Pokemon Party", ENABLE_POKEMON_PARTY != 0, kRequiresPokemonPartyAll,
     sizeof(kRequiresPokemonPartyAll) / sizeof(kRequiresPokemonPartyAll[0]), nullptr, 0},
    {"web_wallpaper_plugin", "Web Wallpaper", ENABLE_WEB_WALLPAPER_PLUGIN != 0, nullptr, 0, nullptr, 0},
    {"anki_support", "Anki Support", ENABLE_ANKI_SUPPORT != 0, nullptr, 0, nullptr, 0},
    {"trmnl_switch", "TRMNL Switch", ENABLE_TRMNL_SWITCH != 0, nullptr, 0, nullptr, 0},
    {"epub_support", "EPUB Support", ENABLE_EPUB_SUPPORT != 0, nullptr, 0, nullptr, 0},
    {"hyphenation", "Hyphenation", ENABLE_HYPHENATION != 0, kRequiresHyphenationAll,
     sizeof(kRequiresHyphenationAll) / sizeof(kRequiresHyphenationAll[0]), nullptr, 0},
    {"xtc_support", "XTC Support", ENABLE_XTC_SUPPORT != 0, nullptr, 0, nullptr, 0},
    {"lyra_theme", "Lyra Theme", ENABLE_LYRA_THEME != 0, kRequiresLyraThemeAll,
     sizeof(kRequiresLyraThemeAll) / sizeof(kRequiresLyraThemeAll[0]), nullptr, 0},
    {"ota_updates", "OTA Updates", ENABLE_OTA_UPDATES != 0, nullptr, 0, nullptr, 0},
    {"todo_planner", "Todo Planner", ENABLE_TODO_PLANNER != 0, nullptr, 0, nullptr, 0},
    {"dark_mode", "Dark Mode", ENABLE_DARK_MODE != 0, nullptr, 0, nullptr, 0},
    {"visual_cover_picker", "Visual Cover Picker", ENABLE_VISUAL_COVER_PICKER != 0, nullptr, 0, nullptr, 0},
    {"ble_wifi_provisioning", "BLE WiFi Provisioning", ENABLE_BLE_WIFI_PROVISIONING != 0,
     kRequiresBleWifiProvisioningAll,
     sizeof(kRequiresBleWifiProvisioningAll) / sizeof(kRequiresBleWifiProvisioningAll[0]), nullptr, 0},
    {"user_fonts", "User Fonts", ENABLE_USER_FONTS != 0, nullptr, 0, nullptr, 0},
    {"web_wifi_setup", "Web WiFi Setup", ENABLE_WEB_WIFI_SETUP != 0, nullptr, 0, nullptr, 0},
    {"usb_mass_storage", "USB Mass Storage", ENABLE_USB_MASS_STORAGE != 0, nullptr, 0, nullptr, 0},
};

constexpr size_t kFeatureCount = sizeof(kFeatureCatalog) / sizeof(kFeatureCatalog[0]);

String joinDependencies(const char* const* dependencies, const size_t count, const char* separator) {
  String result;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0) {
      result += separator;
    }
    result += dependencies[i];
  }
  return result;
}

String escapeJsonString(const char* input) {
  if (input == nullptr) {
    return "";
  }

  String escaped;
  for (const char* p = input; *p != '\0'; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    switch (ch) {
      case '\"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (ch < 0x20) {
          char buffer[7];
          snprintf(buffer, sizeof(buffer), "\\u%04x", ch);
          escaped += buffer;
        } else {
          escaped += static_cast<char>(ch);
        }
        break;
    }
  }

  return escaped;
}

}  // namespace

const FeatureDescriptor* FeatureCatalog::all(size_t& count) {
  count = kFeatureCount;
  return kFeatureCatalog;
}

size_t FeatureCatalog::totalCount() { return kFeatureCount; }

const FeatureDescriptor* FeatureCatalog::find(const char* key) {
  if (key == nullptr) {
    return nullptr;
  }

  const auto it = std::find_if(std::begin(kFeatureCatalog), std::end(kFeatureCatalog),
                               [key](const FeatureDescriptor& feature) { return std::strcmp(feature.key, key) == 0; });
  return it == std::end(kFeatureCatalog) ? nullptr : &(*it);
}

bool FeatureCatalog::isEnabled(const char* key) {
  const auto* feature = find(key);
  return feature != nullptr && feature->enabled;
}

int FeatureCatalog::enabledCount() {
  return static_cast<int>(std::count_if(std::begin(kFeatureCatalog), std::end(kFeatureCatalog),
                                        [](const FeatureDescriptor& feature) { return feature.enabled; }));
}

String FeatureCatalog::buildString() {
  String build;
  bool first = true;
  for (const auto& feature : kFeatureCatalog) {
    if (!feature.enabled) {
      continue;
    }
    if (!first) {
      build += ",";
    }
    build += feature.key;
    first = false;
  }
  return build.isEmpty() ? String("lean") : build;
}

String FeatureCatalog::toJson() {
  String json = "{";
  for (size_t i = 0; i < kFeatureCount; ++i) {
    if (i > 0) {
      json += ",";
    }
    json += "\"";
    json += escapeJsonString(kFeatureCatalog[i].key);
    json += "\":";
    json += kFeatureCatalog[i].enabled ? "true" : "false";
  }
  json += "}";
  return json;
}

bool FeatureCatalog::validate(String* error) {
  for (const auto& feature : kFeatureCatalog) {
    if (!feature.enabled) {
      continue;
    }

    for (size_t i = 0; i < feature.requiresAllCount; ++i) {
      const auto* dependency = find(feature.requiresAll[i]);
      if (dependency == nullptr || !dependency->enabled) {
        if (error != nullptr) {
          *error = String(feature.key) + " requires " + feature.requiresAll[i];
        }
        return false;
      }
    }

    if (feature.requiresAnyCount > 0) {
      bool anyEnabled = false;
      for (size_t i = 0; i < feature.requiresAnyCount; ++i) {
        const auto* dependency = find(feature.requiresAny[i]);
        if (dependency != nullptr && dependency->enabled) {
          anyEnabled = true;
          break;
        }
      }
      if (!anyEnabled) {
        if (error != nullptr) {
          *error = String(feature.key) +
                   " requires one of: " + joinDependencies(feature.requiresAny, feature.requiresAnyCount, ", ");
        }
        return false;
      }
    }
  }

  if (error != nullptr) {
    *error = "";
  }
  return true;
}

void FeatureCatalog::printToSerial() {
  LOG_INF("FEATURES", "CrossPoint Reader build configuration:");
  for (const auto& feature : kFeatureCatalog) {
    LOG_INF("FEATURES", "  %-22s %s [%s]", feature.label, feature.enabled ? "ENABLED " : "DISABLED", feature.key);
  }

  LOG_INF("FEATURES", "%d/%d compile-time features enabled", enabledCount(), static_cast<int>(kFeatureCount));

  String dependencyError;
  const bool dependenciesValid = validate(&dependencyError);
  if (dependenciesValid) {
    LOG_INF("FEATURES", "Dependency graph: valid");
  } else {
    LOG_ERR("FEATURES", "Dependency graph: invalid (%s)", dependencyError.c_str());
  }

  LOG_INF("FEATURES", "Build: %s", buildString().c_str());
}

}  // namespace core
