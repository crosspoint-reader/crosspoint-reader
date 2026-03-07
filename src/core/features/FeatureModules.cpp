#include "core/features/FeatureModules.h"

#include <ArduinoJson.h>
#include <FeatureFlags.h>
#include <Logging.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
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
#if ENABLE_EPUB_SUPPORT
#include "activities/reader/EpubReaderActivity.h"
#endif
#if ENABLE_MARKDOWN
#include "activities/reader/MarkdownReaderActivity.h"
#endif
#include "activities/reader/TxtReaderActivity.h"
#if ENABLE_XTC_SUPPORT
#include "activities/reader/XtcReaderActivity.h"
#endif
#include "activities/settings/ButtonRemapActivity.h"
#include "activities/settings/ClearCacheActivity.h"
#include "activities/settings/FactoryResetActivity.h"
#include "activities/settings/LanguageSelectActivity.h"
#include "activities/settings/SettingsActivity.h"
#include "activities/settings/ValidateSleepImagesActivity.h"
#include "core/features/FeatureCatalog.h"
#include "util/StringUtils.h"
#if ENABLE_MARKDOWN
#include "Markdown.h"
#endif
#include "Txt.h"
#if ENABLE_XTC_SUPPORT
#include "Xtc.h"
#endif

#if ENABLE_WEB_POKEDEX_PLUGIN
#include "network/html/PokedexPluginPageHtml.generated.h"
#endif

#if ENABLE_WEB_WALLPAPER_PLUGIN
#include "network/html/WallpaperPluginPageHtml.generated.h"
#endif

#if ENABLE_ANKI_SUPPORT
#include "network/html/AnkiPluginPageHtml.generated.h"
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

#if ENABLE_OTA_UPDATES
#include "activities/settings/OtaUpdateActivity.h"
#include "network/OtaUpdater.h"
#endif

#if ENABLE_USER_FONTS
#include "UserFontManager.h"
#endif

#if ENABLE_TODO_PLANNER
#include "activities/todo/TodoActivity.h"
#include "activities/todo/TodoFallbackActivity.h"
#endif

namespace core {

#if ENABLE_WEB_POKEDEX_PLUGIN
static_assert(PokedexPluginPageHtmlCompressedSize == sizeof(PokedexPluginPageHtml),
              "Pokedex page compressed size mismatch");
#endif

namespace {
bool isEpubDocumentPath(const std::string& path) { return StringUtils::checkFileExtension(path, ".epub"); }

bool isXtcDocumentPath(const std::string& path) {
  return StringUtils::checkFileExtension(path, ".xtc") || StringUtils::checkFileExtension(path, ".xtch");
}

bool isTxtDocumentPath(const std::string& path) { return StringUtils::checkFileExtension(path, ".txt"); }

bool isMarkdownDocumentPath(const std::string& path) { return StringUtils::checkFileExtension(path, ".md"); }

std::string fileNameFromPath(const std::string& path) {
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash == std::string::npos) {
    return path;
  }
  return path.substr(lastSlash + 1);
}

#if ENABLE_EPUB_SUPPORT
std::unique_ptr<Epub> loadEpubDocument(const std::string& path) {
  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load EPUB");
  return nullptr;
}
#endif

#if ENABLE_XTC_SUPPORT
std::unique_ptr<Xtc> loadXtcDocument(const std::string& path) {
  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}
#endif

std::unique_ptr<Txt> loadTxtDocument(const std::string& path) {
  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = std::unique_ptr<Txt>(new Txt(path, "/.crosspoint"));
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

#if ENABLE_MARKDOWN
std::unique_ptr<Markdown> loadMarkdownDocument(const std::string& path) {
  SpiBusMutex::Guard guard;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto markdown = std::unique_ptr<Markdown>(new Markdown(path, "/.crosspoint"));
  if (markdown->load()) {
    return markdown;
  }

  LOG_ERR("READER", "Failed to load Markdown");
  return nullptr;
}
#endif

#if ENABLE_OTA_UPDATES
enum class OtaWebCheckState { Idle, Checking, Done };

struct OtaWebCheckData {
  std::atomic<OtaWebCheckState> state{OtaWebCheckState::Idle};
  bool available = false;
  std::string latestVersion;
  std::string message;
  int errorCode = 0;
};

OtaWebCheckData otaWebCheckData;
std::mutex otaWebCheckDataMutex;

void otaWebCheckTask(void* param) {
  auto* updater = static_cast<OtaUpdater*>(param);
  const auto result = updater->checkForUpdate();

  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    otaWebCheckData.errorCode = static_cast<int>(result);
    if (result == OtaUpdater::OK) {
      otaWebCheckData.available = updater->isUpdateNewer();
      otaWebCheckData.latestVersion = updater->getLatestVersion();
      otaWebCheckData.message =
          otaWebCheckData.available ? "Update available. Install from device Settings." : "Already on latest version.";
    } else {
      otaWebCheckData.available = false;
      const String& error = updater->getLastError();
      otaWebCheckData.message = error.length() > 0 ? error.c_str() : "Update check failed";
    }
  }

  otaWebCheckData.state.store(OtaWebCheckState::Done, std::memory_order_release);
  delete updater;
  vTaskDelete(nullptr);
}
#endif
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

FeatureModules::ReaderOpenResult FeatureModules::createReaderActivityForPath(
    const std::string& path, GfxRenderer& renderer, MappedInputManager& mappedInput,
    const std::function<void(const std::string&)>& onBackToLibraryPath, const std::function<void()>& onBackHome) {
  (void)onBackToLibraryPath;
  (void)onBackHome;

  if (path.empty()) {
    return {};
  }

  if (isXtcDocumentPath(path)) {
    if (!hasCapability(Capability::XtcSupport)) {
      return {ReaderOpenStatus::Unsupported, nullptr, "XTC support disabled in this build",
              "XTC support\nnot available\nin this build"};
    }
#if ENABLE_XTC_SUPPORT
    auto xtc = loadXtcDocument(path);
    if (!xtc) {
      return {};
    }

    return {
        ReaderOpenStatus::Opened,
        new XtcReaderActivity(renderer, mappedInput, std::move(xtc)),
        nullptr,
        nullptr,
    };
#else
    return {ReaderOpenStatus::Unsupported, nullptr, "XTC support disabled in this build",
            "XTC support\nnot available\nin this build"};
#endif
  }

  if (isMarkdownDocumentPath(path)) {
    if (!hasCapability(Capability::MarkdownSupport)) {
      return {ReaderOpenStatus::Unsupported, nullptr, "Markdown support disabled in this build",
              "Markdown support\nnot available\nin this build"};
    }
#if ENABLE_MARKDOWN
    auto markdown = loadMarkdownDocument(path);
    if (!markdown) {
      return {};
    }

    const std::string markdownPath = markdown->getPath();
    return {
        ReaderOpenStatus::Opened,
        new MarkdownReaderActivity(
            renderer, mappedInput, std::move(markdown),
            [onBackToLibraryPath, markdownPath] { onBackToLibraryPath(markdownPath); }, onBackHome),
        nullptr,
        nullptr,
    };
#else
    return {ReaderOpenStatus::Unsupported, nullptr, "Markdown support disabled in this build",
            "Markdown support\nnot available\nin this build"};
#endif
  }

  if (isTxtDocumentPath(path)) {
    auto txt = loadTxtDocument(path);
    if (!txt) {
      return {};
    }

    return {
        ReaderOpenStatus::Opened,
        new TxtReaderActivity(renderer, mappedInput, std::move(txt)),
        nullptr,
        nullptr,
    };
  }

  if (!isEpubDocumentPath(path)) {
    return {ReaderOpenStatus::Unsupported, nullptr, "Unsupported format", "Unsupported\nformat"};
  }

  if (!hasCapability(Capability::EpubSupport)) {
    return {ReaderOpenStatus::Unsupported, nullptr, "EPUB support disabled in this build",
            "EPUB support\nnot available\nin this build"};
  }

#if ENABLE_EPUB_SUPPORT
  auto epub = loadEpubDocument(path);
  if (!epub) {
    return {};
  }

  return {
      ReaderOpenStatus::Opened,
      new EpubReaderActivity(renderer, mappedInput, std::move(epub)),
      nullptr,
      nullptr,
  };
#else
  return {ReaderOpenStatus::Unsupported, nullptr, "EPUB support disabled in this build",
          "EPUB support\nnot available\nin this build"};
#endif
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

  if (isTxtDocumentPath(path) || isMarkdownDocumentPath(path)) {
    result.handled = true;
    result.title = fileNameFromPath(path);
    return result;
  }

  return result;
}

bool FeatureModules::isSupportedLibraryFile(const std::string& path) {
  if (path.empty()) {
    return false;
  }

  if (isEpubDocumentPath(path)) {
    return hasCapability(Capability::EpubSupport);
  }

  if (isXtcDocumentPath(path)) {
    return hasCapability(Capability::XtcSupport);
  }

  if (isMarkdownDocumentPath(path)) {
    return hasCapability(Capability::MarkdownSupport);
  }

  if (isTxtDocumentPath(path)) {
    return true;
  }

  return StringUtils::checkFileExtension(path, ".bmp");
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
      return hasCapability(Capability::KoreaderSync);
    case SettingAction::OPDSBrowser:
      return hasCapability(Capability::CalibreSync);
    case SettingAction::PokemonParty:
      return hasCapability(Capability::PokemonParty);
    case SettingAction::CheckForUpdates:
      return hasCapability(Capability::OtaUpdates);
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
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
      return new KOReaderSettingsActivity(renderer, mappedInput);
#else
      return nullptr;
#endif
    case SettingAction::OPDSBrowser:
#if ENABLE_INTEGRATIONS && ENABLE_CALIBRE_SYNC
      return new CalibreSettingsActivity(renderer, mappedInput);
#else
      return nullptr;
#endif
    case SettingAction::CheckForUpdates:
#if ENABLE_OTA_UPDATES
      return new OtaUpdateActivity(renderer, mappedInput);
#else
      return nullptr;
#endif
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
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  if (!hasCapability(Capability::KoreaderSync)) {
    return false;
  }
  return KOREADER_STORE.hasCredentials();
#else
  return false;
#endif
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
                                                    std::string filePath, std::string dateTitle,
                                                    const std::function<void()>& onBack) {
#if ENABLE_TODO_PLANNER
  if (!hasCapability(Capability::TodoPlanner)) {
    return nullptr;
  }
  return new TodoActivity(renderer, mappedInput, std::move(filePath), std::move(dateTitle), onBack);
#else
  (void)renderer;
  (void)mappedInput;
  (void)filePath;
  (void)dateTitle;
  (void)onBack;
  return nullptr;
#endif
}

Activity* FeatureModules::createTodoFallbackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     std::string dateText, const std::function<void()>& onBack) {
#if ENABLE_TODO_PLANNER
  if (!hasCapability(Capability::TodoPlanner)) {
    return nullptr;
  }
  return new TodoFallbackActivity(renderer, mappedInput, std::move(dateText), onBack);
#else
  (void)renderer;
  (void)mappedInput;
  (void)dateText;
  (void)onBack;
  return nullptr;
#endif
}

std::string FeatureModules::getKoreaderUsername() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  return KOREADER_STORE.getUsername();
#else
  return "";
#endif
}

std::string FeatureModules::getKoreaderPassword() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  return KOREADER_STORE.getPassword();
#else
  return "";
#endif
}

std::string FeatureModules::getKoreaderServerUrl() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  return KOREADER_STORE.getServerUrl();
#else
  return "";
#endif
}

uint8_t FeatureModules::getKoreaderMatchMethod() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  return static_cast<uint8_t>(KOREADER_STORE.getMatchMethod());
#else
  return 0;
#endif
}

void FeatureModules::setKoreaderUsername(const std::string& username, const bool save) {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  KOREADER_STORE.setCredentials(username, KOREADER_STORE.getPassword());
  if (save) {
    KOREADER_STORE.saveToFile();
  }
#else
  (void)username;
  (void)save;
#endif
}

void FeatureModules::setKoreaderPassword(const std::string& password, const bool save) {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  KOREADER_STORE.setCredentials(KOREADER_STORE.getUsername(), password);
  if (save) {
    KOREADER_STORE.saveToFile();
  }
#else
  (void)password;
  (void)save;
#endif
}

void FeatureModules::setKoreaderServerUrl(const std::string& serverUrl, const bool save) {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  KOREADER_STORE.setServerUrl(serverUrl);
  if (save) {
    KOREADER_STORE.saveToFile();
  }
#else
  (void)serverUrl;
  (void)save;
#endif
}

void FeatureModules::setKoreaderMatchMethod(const uint8_t method, const bool save) {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  const auto selectedMethod = method == static_cast<uint8_t>(DocumentMatchMethod::BINARY)
                                  ? DocumentMatchMethod::BINARY
                                  : DocumentMatchMethod::FILENAME;
  KOREADER_STORE.setMatchMethod(selectedMethod);
  if (save) {
    KOREADER_STORE.saveToFile();
  }
#else
  (void)method;
  (void)save;
#endif
}

void FeatureModules::saveKoreaderSettings() {
#if ENABLE_INTEGRATIONS && ENABLE_KOREADER_SYNC
  KOREADER_STORE.saveToFile();
#endif
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
#if ENABLE_USER_FONTS
  auto& fontManager = UserFontManager::getInstance();
  if (newValue == CrossPointSettings::USER_SD) {
    if (!fontManager.loadFontFamily(SETTINGS.userFontPath)) {
      SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
    }
  } else {
    fontManager.unloadCurrentFont();
  }
#else
  (void)newValue;
#endif
}

void FeatureModules::onWebSettingsApplied() {
#if ENABLE_USER_FONTS
  if (SETTINGS.fontFamily == CrossPointSettings::USER_SD &&
      !UserFontManager::getInstance().loadFontFamily(SETTINGS.userFontPath)) {
    SETTINGS.fontFamily = CrossPointSettings::BOOKERLY;
  }
#endif
}

void FeatureModules::onUploadCompleted(const String& uploadPath, const String& uploadFileName) {
#if ENABLE_USER_FONTS
  String normalizedUploadFileName = uploadFileName;
  normalizedUploadFileName.toLowerCase();
  if (uploadPath == "/fonts" && normalizedUploadFileName.endsWith(".cpf")) {
    auto& manager = UserFontManager::getInstance();
    manager.invalidateCache();
    manager.scanFonts();
  }
#else
  (void)uploadPath;
  (void)uploadFileName;
#endif
}

void FeatureModules::onWebFileChanged(const String& filePath) {
#if ENABLE_EPUB_SUPPORT
  if (StringUtils::checkFileExtension(filePath, ".epub")) {
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

FeatureModules::WebCompressedPayload FeatureModules::getPokedexPluginPagePayload() {
#if ENABLE_WEB_POKEDEX_PLUGIN
  return {true, PokedexPluginPageHtml, PokedexPluginPageHtmlCompressedSize};
#else
  return {false, nullptr, 0};
#endif
}

FeatureModules::WebCompressedPayload FeatureModules::getWallpaperPluginPagePayload() {
#if ENABLE_WEB_WALLPAPER_PLUGIN
  return {true, WallpaperPluginPageHtml, WallpaperPluginPageHtmlCompressedSize};
#else
  return {false, nullptr, 0};
#endif
}

FeatureModules::WebCompressedPayload FeatureModules::getAnkiPluginPagePayload() {
#if ENABLE_ANKI_SUPPORT
  return {true, AnkiPluginPageHtml, AnkiPluginPageHtmlCompressedSize};
#else
  return {false, nullptr, 0};
#endif
}

FeatureModules::OtaWebStartResult FeatureModules::startOtaWebCheck() {
#if ENABLE_OTA_UPDATES
  if (otaWebCheckData.state.load(std::memory_order_acquire) == OtaWebCheckState::Checking) {
    return OtaWebStartResult::AlreadyChecking;
  }

  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    otaWebCheckData.available = false;
    otaWebCheckData.latestVersion.clear();
    otaWebCheckData.message = "Checking...";
    otaWebCheckData.errorCode = 0;
  }
  otaWebCheckData.state.store(OtaWebCheckState::Checking, std::memory_order_release);

  auto* updater = new OtaUpdater();
  if (xTaskCreate(otaWebCheckTask, "OtaWebCheckTask", 4096, updater, 1, nullptr) != pdPASS) {
    delete updater;
    otaWebCheckData.state.store(OtaWebCheckState::Idle, std::memory_order_release);
    return OtaWebStartResult::StartTaskFailed;
  }

  return OtaWebStartResult::Started;
#else
  return OtaWebStartResult::Disabled;
#endif
}

FeatureModules::OtaWebCheckSnapshot FeatureModules::getOtaWebCheckSnapshot() {
#if ENABLE_OTA_UPDATES
  OtaWebCheckSnapshot snapshot;
  const OtaWebCheckState state = otaWebCheckData.state.load(std::memory_order_acquire);
  snapshot.status = state == OtaWebCheckState::Checking
                        ? OtaWebCheckStatus::Checking
                        : (state == OtaWebCheckState::Done ? OtaWebCheckStatus::Done : OtaWebCheckStatus::Idle);
  {
    std::lock_guard<std::mutex> lock(otaWebCheckDataMutex);
    snapshot.available = otaWebCheckData.available;
    snapshot.latestVersion = otaWebCheckData.latestVersion;
    snapshot.message = otaWebCheckData.message;
    snapshot.errorCode = otaWebCheckData.errorCode;
  }
  return snapshot;
#else
  return {};
#endif
}

FeatureModules::FontScanResult FeatureModules::onFontScanRequested() {
#if ENABLE_USER_FONTS
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
  return {true, static_cast<int>(fontManager.getAvailableFonts().size()), activeLoaded};
#else
  return {false, 0, false};
#endif
}

bool FeatureModules::shouldExposeHomeAction(const HomeOptionalAction action, const bool hasOpdsUrl) {
  switch (action) {
    case HomeOptionalAction::AnkiSupport:
      return hasCapability(Capability::AnkiSupport);
    case HomeOptionalAction::OpdsBrowser:
      return hasCapability(Capability::CalibreSync) && hasOpdsUrl;
    case HomeOptionalAction::TodoPlanner:
      return hasCapability(Capability::TodoPlanner);
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
