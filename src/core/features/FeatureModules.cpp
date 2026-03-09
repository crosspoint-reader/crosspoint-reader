#include "core/features/FeatureModules.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>

#if ENABLE_EPUB_SUPPORT
#include <Epub.h>
#endif

// Standard thumbnail height for cover images (matches HomeActivity: screenHeight/2 on 480px display).
// This height is used wherever a cover BMP is referenced outside of a specific render context.
static constexpr int kDefaultThumbHeight = 240;

#include "CrossPointSettings.h"
#include "SpiBusMutex.h"
#include "activities/home/RecentBooksActivity.h"
#include "activities/network/WifiSelectionActivity.h"
#include "activities/settings/ButtonRemapActivity.h"
#include "activities/settings/ClearCacheActivity.h"
#include "activities/settings/FactoryResetActivity.h"
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/ValidateSleepImagesActivity.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/HomeActionRegistry.h"
#include "core/registries/LifecycleRegistry.h"
#include "core/registries/ReaderRegistry.h"
#include "core/registries/SettingsActionRegistry.h"
#include "core/registries/SyncServiceRegistry.h"
#include "util/StringUtils.h"
#if ENABLE_XTC_SUPPORT
#include "Xtc.h"
#endif

#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
#include "activities/browser/OpdsBookBrowserActivity.h"
#include "activities/settings/CalibreSettingsActivity.h"
#endif

#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
#include "KOReaderCredentialStore.h"
#include "activities/reader/KOReaderSyncActivity.h"
#include "activities/settings/KOReaderSettingsActivity.h"
#endif

#if ENABLE_USER_FONTS
#include "UserFontManager.h"
#endif

#if ENABLE_TODO_PLANNER
#include "activities/todo/TodoActivity.h"
#include "activities/todo/TodoFallbackActivity.h"
#endif

namespace core {

namespace {
bool isEpubDocumentPath(const std::string& path) { return FsHelpers::checkFileExtension(path, ".epub"); }

bool isXtcDocumentPath(const std::string& path) {
  return FsHelpers::checkFileExtension(path, ".xtc") || FsHelpers::checkFileExtension(path, ".xtch");
}

bool isMarkdownDocumentPath(const std::string& path) { return FsHelpers::checkFileExtension(path, ".md"); }

std::string fileNameFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos) {
    return path;
  }
  return path.substr(lastSlash + 1);
}

}  // namespace

bool FeatureModules::isEnabled(const char* featureKey) { return FeatureCatalog::isEnabled(featureKey); }

bool FeatureModules::hasCapability(const Capability capability) {
  switch (capability) {
    case Capability::AnkiSupport:
      return isEnabled("anki_support");
    case Capability::BackgroundServer:
      return isEnabled("background_server");
    case Capability::BleWifiProvisioning:
      return isEnabled("ble_wifi_provisioning");
    case Capability::CalibreSync:
      return isEnabled("calibre_sync");
    case Capability::DarkMode:
      return isEnabled("dark_mode");
    case Capability::EpubSupport:
      return isEnabled("epub_support");
    case Capability::HomeMediaPicker:
      return isEnabled("home_media_picker");
    case Capability::KoreaderSync:
      return isEnabled("koreader_sync");
    case Capability::LyraTheme:
      return isEnabled("lyra_theme");
    case Capability::MarkdownSupport:
      return isEnabled("markdown");
    case Capability::OtaUpdates:
      return isEnabled("ota_updates");
    case Capability::PokemonParty:
      return isEnabled("pokemon_party");
    case Capability::RemoteOpenBook:
      return true;
    case Capability::RemotePageTurn:
      return true;
    case Capability::TodoPlanner:
      return isEnabled("todo_planner");
    case Capability::TrmnlSwitch:
      return isEnabled("trmnl_switch");
    case Capability::UsbMassStorage:
      return isEnabled("usb_mass_storage");
    case Capability::UserFonts:
      return isEnabled("user_fonts");
    case Capability::VisualCoverPicker:
      return isEnabled("visual_cover_picker");
    case Capability::WebPokedexPlugin:
      return isEnabled("web_pokedex_plugin");
    case Capability::WebWallpaperPlugin:
      return isEnabled("web_wallpaper_plugin");
    case Capability::WebWifiSetup:
      return isEnabled("web_wifi_setup");
    case Capability::XtcSupport:
      return isEnabled("xtc_support");
    default:
      return false;
  }
}

String FeatureModules::getBuildString() { return FeatureCatalog::buildString(); }

String FeatureModules::getFeatureMapJson() {
  JsonDocument doc;
  deserializeJson(doc, FeatureCatalog::toJson());
  doc["remote_open_book"] = hasCapability(Capability::RemoteOpenBook);
  doc["remote_page_turn"] = hasCapability(Capability::RemotePageTurn);

  String json;
  serializeJson(doc, json);
  return json;
}

FeatureModules::HomeCardDataResult FeatureModules::resolveHomeCardData(const std::string& path, const int thumbHeight) {
  HomeCardDataResult result;
  if (path.empty() || thumbHeight <= 0) {
    return result;
  }

  if (isEpubDocumentPath(path)) {
    result.handled = true;
    if (!hasCapability(Capability::EpubSupport)) {
      return result;
    }
#if ENABLE_EPUB_SUPPORT
    Epub epub(path, "/.crosspoint");
    if (!epub.load(false, true)) {
      return result;
    }

    result.loaded = true;
    if (!epub.getTitle().empty()) {
      result.title = epub.getTitle();
    }
    if (!epub.getAuthor().empty()) {
      result.author = epub.getAuthor();
    }
    if (epub.generateThumbBmp(thumbHeight)) {
      result.coverPath = epub.getThumbBmpPath(thumbHeight);
    }
#endif
    return result;
  }

  if (isXtcDocumentPath(path)) {
    result.handled = true;
    if (!hasCapability(Capability::XtcSupport)) {
      return result;
    }
#if ENABLE_XTC_SUPPORT
    Xtc xtc(path, "/.crosspoint");
    if (!xtc.load()) {
      return result;
    }

    result.loaded = true;
    const std::string xtcTitle = xtc.getTitle();
    if (!xtcTitle.empty()) {
      result.title = xtcTitle;
    }
    const std::string xtcAuthor = xtc.getAuthor();
    if (!xtcAuthor.empty()) {
      result.author = xtcAuthor;
    }
    if (xtc.generateThumbBmp(thumbHeight)) {
      result.coverPath = xtc.getThumbBmpPath(thumbHeight);
    }
#endif
    return result;
  }

  return result;
}

FeatureModules::RecentBookDataResult FeatureModules::resolveRecentBookData(const std::string& path) {
  RecentBookDataResult result;
  if (path.empty()) {
    return result;
  }

  if (isEpubDocumentPath(path)) {
    result.handled = true;
    if (!hasCapability(Capability::EpubSupport)) {
      return result;
    }
#if ENABLE_EPUB_SUPPORT
    Epub epub(path, "/.crosspoint");
    // Match resolveHomeCardData behavior: only expose metadata when the file loaded.
    if (!epub.load(false)) {
      return result;
    }
    result.title = epub.getTitle();
    result.author = epub.getAuthor();
    result.coverPath = epub.getThumbBmpPath(kDefaultThumbHeight);
#endif
    return result;
  }

  if (isXtcDocumentPath(path)) {
    result.handled = true;
    if (!hasCapability(Capability::XtcSupport)) {
      return result;
    }
#if ENABLE_XTC_SUPPORT
    Xtc xtc(path, "/.crosspoint");
    if (!xtc.load()) {
      return result;
    }

    result.title = xtc.getTitle();
    result.author = xtc.getAuthor();
    result.coverPath = xtc.getThumbBmpPath(kDefaultThumbHeight);
#endif
    return result;
  }

  if (FsHelpers::checkFileExtension(path, ".txt") || isMarkdownDocumentPath(path)) {
    result.handled = true;
    result.title = fileNameFromPath(path);
    return result;
  }

  return result;
}

bool FeatureModules::isSupportedLibraryFile(const std::string& path) {
  return ReaderRegistry::isSupportedLibraryFile(path);
}

bool FeatureModules::supportsSettingAction(const SettingAction action) {
  switch (action) {
    case SettingAction::RemapFrontButtons:
    case SettingAction::Network:
    case SettingAction::ClearCache:
    case SettingAction::FactoryReset:
    case SettingAction::ValidateSleepImages:
      return true;
    case SettingAction::KOReaderSync:
    case SettingAction::OPDSBrowser:
    case SettingAction::CheckForUpdates:
      return core::SettingsActionRegistry::isSupported(action);
    case SettingAction::PokemonParty:
      return hasCapability(Capability::PokemonParty);
    case SettingAction::SwitchToTrmnl:
      return hasCapability(Capability::TrmnlSwitch);
    case SettingAction::Language:
      return true;
    case SettingAction::None:
      return false;
  }
  return false;
}

Activity* FeatureModules::createSettingsSubActivity(const SettingAction action, GfxRenderer& renderer,
                                                    MappedInputManager& mappedInput,
                                                    const std::function<void()>& onComplete,
                                                    const std::function<void(bool)>& onCompleteBool) {
  if (!supportsSettingAction(action)) {
    return nullptr;
  }

  switch (action) {
    case SettingAction::RemapFrontButtons:
      return new ButtonRemapActivity(renderer, mappedInput);
    case SettingAction::Network:
      return new WifiSelectionActivity(renderer, mappedInput, false);
    case SettingAction::ClearCache:
      return new ClearCacheActivity(renderer, mappedInput);
    case SettingAction::FactoryReset:
      return new FactoryResetActivity(renderer, mappedInput, onComplete);
    case SettingAction::PokemonParty:
      return new RecentBooksActivity(renderer, mappedInput);
    case SettingAction::KOReaderSync:
    case SettingAction::OPDSBrowser:
    case SettingAction::CheckForUpdates:
      return core::SettingsActionRegistry::create(action, renderer, mappedInput, nullptr, nullptr, nullptr);
    case SettingAction::Language:
      return new LanguageSelectActivity(renderer, mappedInput);
    case SettingAction::ValidateSleepImages:
      return new ValidateSleepImagesActivity(renderer, mappedInput, onComplete);
    case SettingAction::None:
      return nullptr;
  }

  return nullptr;
}

Activity* FeatureModules::createOpdsBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                    const std::function<void()>& onBack) {
#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
  if (!hasCapability(Capability::CalibreSync)) {
    return nullptr;
  }
  (void)onBack;
  return new OpdsBookBrowserActivity(renderer, mappedInput);
#else
  (void)renderer;
  (void)mappedInput;
  (void)onBack;
  return nullptr;
#endif
}

bool FeatureModules::hasKoreaderSyncCredentials() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api == nullptr || api->hasCredentials == nullptr) {
    return false;
  }
  return api->hasCredentials();
}

Activity* FeatureModules::createKoreaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                                     const int currentSpineIndex, const int currentPage,
                                                     const int totalPagesInSpine, const std::function<void()>& onCancel,
                                                     const std::function<void(int, int)>& onSyncComplete) {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  if (!hasKoreaderSyncCredentials()) {
    return nullptr;
  }
  (void)onCancel;
  (void)onSyncComplete;
  return new KOReaderSyncActivity(renderer, mappedInput, epub, epubPath, currentSpineIndex, currentPage,
                                  totalPagesInSpine);
#else
  (void)renderer;
  (void)mappedInput;
  (void)epub;
  (void)epubPath;
  (void)currentSpineIndex;
  (void)currentPage;
  (void)totalPagesInSpine;
  (void)onCancel;
  (void)onSyncComplete;
  return nullptr;
#endif
}

Activity* FeatureModules::createTodoPlannerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                    std::string filePath, std::string dateTitle, void* onBackCtx,
                                                    void (*onBack)(void*)) {
#if ENABLE_TODO_PLANNER
  if (!hasCapability(Capability::TodoPlanner)) {
    return nullptr;
  }
  return new TodoActivity(renderer, mappedInput, std::move(filePath), std::move(dateTitle), onBackCtx, onBack);
#else
  (void)renderer;
  (void)mappedInput;
  (void)filePath;
  (void)dateTitle;
  (void)onBackCtx;
  (void)onBack;
  return nullptr;
#endif
}

Activity* FeatureModules::createTodoFallbackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string dateText, void* onBackCtx, void (*onBack)(void*)) {
#if ENABLE_TODO_PLANNER
  if (!hasCapability(Capability::TodoPlanner)) {
    return nullptr;
  }
  return new TodoFallbackActivity(renderer, mappedInput, std::move(dateText), onBackCtx, onBack);
#else
  (void)renderer;
  (void)mappedInput;
  (void)dateText;
  (void)onBackCtx;
  (void)onBack;
  return nullptr;
#endif
}

std::string FeatureModules::getKoreaderUsername() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api == nullptr || api->getUsername == nullptr) {
    return "";
  }

  char buffer[kSyncCredBufSize] = {};
  api->getUsername(buffer, sizeof(buffer));
  return buffer;
}

std::string FeatureModules::getKoreaderPassword() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api == nullptr || api->getPassword == nullptr) {
    return "";
  }
  char buffer[kSyncCredBufSize] = {};
  api->getPassword(buffer, sizeof(buffer));
  return buffer;
}

std::string FeatureModules::getKoreaderServerUrl() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api == nullptr || api->getServerUrl == nullptr) {
    return "";
  }

  char buffer[kSyncUrlBufSize] = {};
  api->getServerUrl(buffer, sizeof(buffer));
  return buffer;
}

uint8_t FeatureModules::getKoreaderMatchMethod() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api == nullptr || api->getMatchMethod == nullptr) {
    return 0;
  }
  return api->getMatchMethod();
}

void FeatureModules::setKoreaderUsername(const std::string& username, const bool save) {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api != nullptr && api->setUsername != nullptr) {
    api->setUsername(username.c_str(), save);
  }
}

void FeatureModules::setKoreaderPassword(const std::string& password, const bool save) {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api != nullptr && api->setPassword != nullptr) {
    api->setPassword(password.c_str(), save);
  }
}

void FeatureModules::setKoreaderServerUrl(const std::string& serverUrl, const bool save) {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api != nullptr && api->setServerUrl != nullptr) {
    api->setServerUrl(serverUrl.c_str(), save);
  }
}

void FeatureModules::setKoreaderMatchMethod(const uint8_t method, const bool save) {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api != nullptr && api->setMatchMethod != nullptr) {
    api->setMatchMethod(method, save);
  }
}

void FeatureModules::saveKoreaderSettings() {
  const auto* api = SyncServiceRegistry::getKoreaderApi();
  if (api != nullptr && api->saveSettings != nullptr) {
    api->saveSettings();
  }
}

std::vector<std::string> FeatureModules::getUserFontFamilies() {
#if ENABLE_USER_FONTS
  auto& manager = UserFontManager::getInstance();
  manager.ensureScanned();
  return manager.getAvailableFonts();
#else
  return {};
#endif
}

uint8_t FeatureModules::getSelectedUserFontFamilyIndex() {
#if ENABLE_USER_FONTS
  auto& manager = UserFontManager::getInstance();
  manager.ensureScanned();
  const auto& fonts = manager.getAvailableFonts();
  if (fonts.empty()) {
    return 0;
  }

  const std::string selectedFont = SETTINGS.userFontPath;
  const auto it = std::find(fonts.begin(), fonts.end(), selectedFont);
  if (it == fonts.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(fonts.begin(), it));
#else
  return 0;
#endif
}

void FeatureModules::setSelectedUserFontFamilyIndex(const uint8_t index) {
#if ENABLE_USER_FONTS
  auto& manager = UserFontManager::getInstance();
  manager.ensureScanned();
  const auto& fonts = manager.getAvailableFonts();
  if (fonts.empty()) {
    SETTINGS.userFontPath[0] = '\0';
    if (SETTINGS.fontFamily == CrossPointSettings::USER_SD) {
      SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
      manager.unloadCurrentFont();
    }
    return;
  }

  const size_t selectedIndex = std::min(static_cast<size_t>(index), fonts.size() - 1);
  strncpy(SETTINGS.userFontPath, fonts[selectedIndex].c_str(), sizeof(SETTINGS.userFontPath) - 1);
  SETTINGS.userFontPath[sizeof(SETTINGS.userFontPath) - 1] = '\0';
  if (SETTINGS.fontFamily == CrossPointSettings::USER_SD && !manager.loadFontFamily(SETTINGS.userFontPath)) {
    SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
  }
#else
  (void)index;
#endif
}

void FeatureModules::onFontFamilySettingChanged(const uint8_t newValue) {
  LifecycleRegistry::dispatchFontFamilyChanged(newValue);
}

void FeatureModules::onWebSettingsApplied() { LifecycleRegistry::dispatchWebSettingsApplied(); }

void FeatureModules::onUploadCompleted(const String& uploadPath, const String& uploadFileName) {
  LifecycleRegistry::dispatchUploadCompleted(uploadPath.c_str(), uploadFileName.c_str());
}

void FeatureModules::onWebFileChanged(const String& filePath) {
#if ENABLE_EPUB_SUPPORT
  if (FsHelpers::checkFileExtension(filePath, ".epub")) {
    Epub(filePath.c_str(), "/.crosspoint").clearCache();
    LOG_DBG("FEATURES", "Cleared epub cache for: %s", filePath.c_str());
  }
#else
  (void)filePath;
#endif
}

bool FeatureModules::tryGetDocumentCoverPath(const String& documentPath, std::string& outCoverPath) {
#if ENABLE_EPUB_SUPPORT
  String lowerPath = documentPath;
  lowerPath.toLowerCase();
  if (!lowerPath.endsWith(".epub")) {
    return false;
  }

  Epub epub(documentPath.c_str(), "/.crosspoint");
  SpiBusMutex::Guard guard;
  if (!epub.load(false)) {
    return false;
  }

  outCoverPath = epub.getThumbBmpPath(kDefaultThumbHeight);
  return !outCoverPath.empty();
#else
  (void)documentPath;
  outCoverPath.clear();
  return false;
#endif
}
bool FeatureModules::shouldExposeHomeAction(const HomeOptionalAction action, const bool hasOpdsUrl) {
  const core::HomeActionEntry::HomeActionContext ctx{hasOpdsUrl};
  switch (action) {
    case HomeOptionalAction::AnkiSupport:
      return core::HomeActionRegistry::shouldExpose("anki", ctx);
    case HomeOptionalAction::OpdsBrowser:
      return core::HomeActionRegistry::shouldExpose("opds_browser", ctx);
    case HomeOptionalAction::TodoPlanner:
      return core::HomeActionRegistry::shouldExpose("todo_planner", ctx);
  }
  return false;
}

bool FeatureModules::shouldRegisterWebRoute(const WebOptionalRoute route) {
  switch (route) {
    case WebOptionalRoute::PokedexPluginPage:
      return hasCapability(Capability::WebPokedexPlugin);
    case WebOptionalRoute::PokemonPartyApi:
      return hasCapability(Capability::PokemonParty);
    case WebOptionalRoute::WallpaperPluginPage:
      return hasCapability(Capability::WebWallpaperPlugin);
    case WebOptionalRoute::AnkiPluginPage:
      return isEnabled("anki_support");
    case WebOptionalRoute::UserFontsApi:
      return hasCapability(Capability::UserFonts);
    case WebOptionalRoute::WebWifiSetupApi:
      return hasCapability(Capability::WebWifiSetup);
    case WebOptionalRoute::OtaApi:
      return hasCapability(Capability::OtaUpdates);
  }
  return false;
}

}  // namespace core
