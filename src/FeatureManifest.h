#pragma once

#include <Arduino.h>
#include <FeatureFlags.h>

#include "core/features/FeatureCatalog.h"

/**
 * FeatureManifest keeps existing compile-time feature helpers while delegating
 * runtime observability and dependency validation to the core feature API.
 */
class FeatureManifest {
 public:
  // Compile-time feature detection (constexpr for zero runtime cost).
  static constexpr bool hasAnyBundledFontFamily() {
    return ENABLE_BOOKERLY_FONTS != 0 || ENABLE_NOTOSANS_FONTS != 0 || ENABLE_OPENDYSLEXIC_FONTS != 0;
  }

  // Legacy aggregate alias kept for API compatibility with existing tooling.
  static constexpr bool hasExtendedFonts() { return hasAnyBundledFontFamily(); }
  static constexpr bool hasBookerlyFonts() { return ENABLE_BOOKERLY_FONTS != 0; }
  static constexpr bool hasNotoSansFonts() { return ENABLE_NOTOSANS_FONTS != 0; }
  static constexpr bool hasOpenDyslexicFonts() { return ENABLE_OPENDYSLEXIC_FONTS != 0; }
  static constexpr bool hasImageSleep() { return ENABLE_IMAGE_SLEEP != 0; }
  static constexpr bool hasBookImages() { return ENABLE_BOOK_IMAGES != 0; }
  static constexpr bool hasMarkdown() { return ENABLE_MARKDOWN != 0; }
  static constexpr bool hasIntegrations() { return ENABLE_INTEGRATIONS != 0; }
  static constexpr bool hasKOReaderSync() { return ENABLE_KOREADER_SYNC != 0; }
  static constexpr bool hasCalibreSync() { return ENABLE_CALIBRE_SYNC != 0; }
  static constexpr bool hasBackgroundServer() { return ENABLE_BACKGROUND_SERVER != 0; }
  static constexpr bool hasHomeMediaPicker() { return ENABLE_HOME_MEDIA_PICKER != 0; }
  static constexpr bool hasWebPokedexPlugin() { return ENABLE_WEB_POKEDEX_PLUGIN != 0; }
  static constexpr bool hasPokemonParty() { return ENABLE_POKEMON_PARTY != 0; }
  static constexpr bool hasWebWallpaperPlugin() { return ENABLE_WEB_WALLPAPER_PLUGIN != 0; }
  static constexpr bool hasAnkiSupport() { return ENABLE_ANKI_SUPPORT != 0; }
  static constexpr bool hasTrmnlSwitch() { return ENABLE_TRMNL_SWITCH != 0; }
  static constexpr bool hasEpubSupport() { return ENABLE_EPUB_SUPPORT != 0; }
  static constexpr bool hasHyphenation() { return ENABLE_HYPHENATION != 0; }
  static constexpr bool hasXtcSupport() { return ENABLE_XTC_SUPPORT != 0; }
  static constexpr bool hasLyraTheme() { return ENABLE_LYRA_THEME != 0; }
  static constexpr bool hasOtaUpdates() { return ENABLE_OTA_UPDATES != 0; }
  static constexpr bool hasTodoPlanner() { return ENABLE_TODO_PLANNER != 0; }
  static constexpr bool hasDarkMode() { return ENABLE_DARK_MODE != 0; }
  static constexpr bool hasVisualCoverPicker() { return ENABLE_VISUAL_COVER_PICKER != 0; }
  static constexpr bool hasBleWifiProvisioning() { return ENABLE_BLE_WIFI_PROVISIONING != 0; }
  static constexpr bool hasUserFonts() { return ENABLE_USER_FONTS != 0; }
  static constexpr bool hasWebWifiSetup() { return ENABLE_WEB_WIFI_SETUP != 0; }
  static constexpr bool hasUsbMassStorage() { return ENABLE_USB_MASS_STORAGE != 0; }

  static size_t totalFeatureCount() { return core::FeatureCatalog::totalCount(); }
  static int enabledFeatureCount() { return core::FeatureCatalog::enabledCount(); }
  static String getBuildString() { return core::FeatureCatalog::buildString(); }
  static String toJson() { return core::FeatureCatalog::toJson(); }
  static bool hasValidDependencies(String* error = nullptr) { return core::FeatureCatalog::validate(error); }
  static void printToSerial() { core::FeatureCatalog::printToSerial(); }
};
