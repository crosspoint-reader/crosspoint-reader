#include "features/user_fonts/Registration.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <WebServer.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "SpiBusMutex.h"
#include "UserFontManager.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/LifecycleRegistry.h"
#include "core/registries/WebRouteRegistry.h"
#include "fontIds.h"
#include "network/BufferedHttpUpload.h"
#include "util/PathUtils.h"

namespace features::user_fonts {
namespace {

#if ENABLE_USER_FONTS
bool shouldRegisterUserFontsApiRoute() { return core::FeatureCatalog::isEnabled("user_fonts"); }

struct FontScanSnapshot {
  int familyCount;
  bool activeLoaded;
};

network::BufferedHttpUploadSession& uploadSession() { return network::sharedBufferedHttpUploadSession(); }

FontScanSnapshot rescanUserFonts() {
  auto& fontManager = UserFontManager::getInstance();
  fontManager.scanFonts();

  bool activeLoaded = true;
  if (SETTINGS.fontFamily == CrossPointSettings::USER_SD) {
    activeLoaded = fontManager.loadFontFamily(SETTINGS.userFontPath);
    if (!activeLoaded) {
      SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
      if (!SETTINGS.saveToFile()) {
        LOG_WRN("FEATURES", "Failed to persist font fallback after rescan");
      }
    }
  }

  return {static_cast<int>(fontManager.getAvailableFonts().size()), activeLoaded};
}

bool resolveUserFontUploadTarget(WebServer* server, const String& uploadFileName,
                                 network::BufferedHttpUploadTarget& target, String& error) {
  (void)server;

  if (!PathUtils::isValidFilename(uploadFileName) || PathUtils::isProtectedWebComponent(uploadFileName)) {
    error = "Invalid filename";
    return false;
  }

  String lowerName = uploadFileName;
  lowerName.toLowerCase();
  if (!lowerName.endsWith(".cpf")) {
    error = "Only .cpf font files are accepted";
    return false;
  }

  {
    SpiBusMutex::Guard guard;
    if (!Storage.exists("/fonts")) {
      Storage.mkdir("/fonts");
    }
  }

  target.uploadPath = "/fonts";
  target.filePath = String("/fonts/") + uploadFileName;
  return true;
}

const network::BufferedHttpUploadConfig kUserFontUploadConfig = {"FONT",
                                                                 "FONT",
                                                                 "Failed to create font file on SD card",
                                                                 "Failed to write font data - disk may be full",
                                                                 "Failed to write final font data",
                                                                 "Font upload aborted",
                                                                 false,
                                                                 resolveUserFontUploadTarget};

void handleRescanUserFonts(WebServer* server) {
  if (server == nullptr) {
    return;
  }

  const FontScanSnapshot result = rescanUserFonts();

  JsonDocument response;
  response["families"] = result.familyCount;
  response["activeLoaded"] = result.activeLoaded;

  String output;
  serializeJson(response, output);
  server->send(200, "application/json", output);
}

void handleFontUpload(WebServer* server) { uploadSession().handleUpload(server, kUserFontUploadConfig); }

void handleFontUploadPost(WebServer* server) {
  if (server == nullptr) {
    return;
  }

  if (!uploadSession().succeeded()) {
    const String error = uploadSession().error().isEmpty() ? "Upload failed" : uploadSession().error();
    server->send(400, "text/plain", error);
    return;
  }

  const FontScanSnapshot result = rescanUserFonts();

  JsonDocument response;
  response["ok"] = true;
  response["families"] = result.familyCount;
  response["activeLoaded"] = result.activeLoaded;

  String output;
  serializeJson(response, output);
  server->send(200, "application/json", output);
}

void mountUserFontsRoutes(WebServer* server) {
  if (server == nullptr) {
    return;
  }

  server->on("/api/user-fonts/rescan", HTTP_POST, [server] { handleRescanUserFonts(server); });
  server->on(
      "/api/user-fonts/upload", HTTP_POST, [server] { handleFontUploadPost(server); },
      [server] { handleFontUpload(server); });
}
#endif

void onStorageReady() { UserFontManager::getInstance().scanFonts(); }

void onSettingsLoaded(GfxRenderer& renderer) {
  (void)renderer;
  if (SETTINGS.fontFamily == CrossPointSettings::USER_SD &&
      !UserFontManager::getInstance().loadFontFamily(SETTINGS.userFontPath)) {
    SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
    if (!SETTINGS.saveToFile()) {
      LOG_WRN("FEATURES", "Failed to persist font fallback after user font load failure");
    }
  }
}

void onFontSetup(GfxRenderer& renderer) {
  renderer.insertFontFamily(USER_SD_FONT_ID, UserFontManager::getInstance().getFontFamily());
}

void onFontFamilyChanged(uint8_t newValue) {
  auto& fontManager = UserFontManager::getInstance();
  if (newValue == CrossPointSettings::USER_SD) {
    if (!fontManager.loadFontFamily(SETTINGS.userFontPath)) {
      SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
    }
  } else {
    fontManager.unloadCurrentFont();
  }
}

void onWebSettingsApplied() {
  if (SETTINGS.fontFamily == CrossPointSettings::USER_SD &&
      !UserFontManager::getInstance().loadFontFamily(SETTINGS.userFontPath)) {
    SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
  }
}

void onUploadCompleted(const char* uploadPath, const char* uploadFileName) {
  if (uploadPath == nullptr || uploadFileName == nullptr || std::strcmp(uploadPath, "/fonts") != 0) {
    return;
  }

  String normalizedUploadFileName = uploadFileName;
  normalizedUploadFileName.toLowerCase();
  if (!normalizedUploadFileName.endsWith(".cpf")) {
    return;
  }

  auto& manager = UserFontManager::getInstance();
  manager.invalidateCache();
  manager.scanFonts();
}

}  // namespace

void registerFeature() {
#if ENABLE_USER_FONTS
  core::LifecycleEntry entry{};
  entry.onStorageReady = onStorageReady;
  entry.onSettingsLoaded = onSettingsLoaded;
  entry.onFontSetup = onFontSetup;
  entry.onFontFamilyChanged = onFontFamilyChanged;
  entry.onWebSettingsApplied = onWebSettingsApplied;
  entry.onUploadCompleted = onUploadCompleted;
  core::LifecycleRegistry::add(entry);

  core::WebRouteEntry webRouteEntry{};
  webRouteEntry.routeId = "user_fonts_api";
  webRouteEntry.shouldRegister = shouldRegisterUserFontsApiRoute;
  webRouteEntry.mountRoutes = mountUserFontsRoutes;
  core::WebRouteRegistry::add(webRouteEntry);
#endif
}

}  // namespace features::user_fonts
