#pragma once

#include <Arduino.h>

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>

class Activity;
class CrossPointWebServer;
class Epub;
class GfxRenderer;
class MappedInputManager;
class WebServer;
enum class SettingAction;

namespace core {

enum class WebOptionalRoute {
  PokedexPluginPage,
  PokemonPartyApi,
  WallpaperPluginPage,
  AnkiPluginPage,
  UserFontsApi,
  WebWifiSetupApi,
  OtaApi,
};

enum class HomeOptionalAction {
  AnkiSupport,
  OpdsBrowser,
  TodoPlanner,
};

enum class Capability {
  AnkiSupport,
  BackgroundServer,
  BleWifiProvisioning,
  CalibreSync,
  DarkMode,
  EpubSupport,
  HomeMediaPicker,
  KoreaderSync,
  LyraTheme,
  MarkdownSupport,
  OtaUpdates,
  PokemonParty,
  RemoteOpenBook,
  RemotePageTurn,
  TodoPlanner,
  TrmnlSwitch,
  UsbMassStorage,
  UserFonts,
  VisualCoverPicker,
  WebPokedexPlugin,
  WebWallpaperPlugin,
  WebWifiSetup,
  XtcSupport,
};

class FeatureModules {
 public:
  enum class ReaderOpenStatus {
    Opened,
    Unsupported,
    LoadFailed,
  };

  struct ReaderOpenResult {
    // When status == Opened, activity is a newly allocated raw Activity* and ownership
    // transfers to the caller, which is responsible for handing it off or deleting it.
    // For all non-Opened statuses, activity stays nullptr.
    ReaderOpenStatus status = ReaderOpenStatus::LoadFailed;
    Activity* activity = nullptr;
    const char* logMessage = nullptr;
    const char* uiMessage = nullptr;
  };

  struct HomeCardDataResult {
    bool handled = false;
    bool loaded = false;
    std::string title;
    std::string author;
    std::string coverPath;
  };

  struct RecentBookDataResult {
    bool handled = false;
    std::string title;
    std::string author;
    std::string coverPath;
  };

  static bool isEnabled(const char* featureKey);
  static bool hasCapability(Capability capability);
  static String getBuildString();
  static String getFeatureMapJson();
  static bool supportsSettingAction(SettingAction action);
  static ReaderOpenResult createReaderActivityForPath(
      const std::string& path, GfxRenderer& renderer, MappedInputManager& mappedInput,
      const std::function<void(const std::string&)>& onBackToLibraryPath, const std::function<void()>& onBackHome);
  static HomeCardDataResult resolveHomeCardData(const std::string& path, int thumbHeight);
  static RecentBookDataResult resolveRecentBookData(const std::string& path);
  static bool isSupportedLibraryFile(const std::string& path);

  static Activity* createSettingsSubActivity(SettingAction action, GfxRenderer& renderer,
                                             MappedInputManager& mappedInput, const std::function<void()>& onComplete,
                                             const std::function<void(bool)>& onCompleteBool);
  static Activity* createOpdsBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::function<void()>& onBack);
  static bool hasKoreaderSyncCredentials();
  static Activity* createKoreaderSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<Epub>& epub, const std::string& epubPath,
                                              int currentSpineIndex, int currentPage, int totalPagesInSpine,
                                              const std::function<void()>& onCancel,
                                              const std::function<void(int, int)>& onSyncComplete);
  static Activity* createTodoPlannerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             std::string filePath, std::string dateTitle,
                                             const std::function<void()>& onBack);
  static Activity* createTodoFallbackActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              std::string dateText, const std::function<void()>& onBack);

  static void onFontFamilySettingChanged(uint8_t newValue);
  static void onWebSettingsApplied();
  static void onUploadCompleted(const String& uploadPath, const String& uploadFileName);
  static void onWebFileChanged(const String& filePath);
  static bool tryGetDocumentCoverPath(const String& documentPath, std::string& outCoverPath);

  static bool shouldRegisterWebRoute(WebOptionalRoute route);
  static bool shouldExposeHomeAction(HomeOptionalAction action, bool hasOpdsUrl);

  static std::string getKoreaderUsername();
  static std::string getKoreaderPassword();
  static std::string getKoreaderServerUrl();
  static uint8_t getKoreaderMatchMethod();
  static void setKoreaderUsername(const std::string& username, bool save = true);
  static void setKoreaderPassword(const std::string& password, bool save = true);
  static void setKoreaderServerUrl(const std::string& serverUrl, bool save = true);
  static void setKoreaderMatchMethod(uint8_t method, bool save = true);
  static void saveKoreaderSettings();
  static std::vector<std::string> getUserFontFamilies();
  static uint8_t getSelectedUserFontFamilyIndex();
  static void setSelectedUserFontFamilyIndex(uint8_t index);

  struct FontScanResult {
    bool available;  // false when ENABLE_USER_FONTS is off
    int familyCount;
    bool activeLoaded;
  };

  struct WebCompressedPayload {
    bool available;
    const char* data;
    size_t compressedSize;
  };

  enum class OtaWebStartResult {
    Disabled,
    Started,
    AlreadyChecking,
    StartTaskFailed,
  };

  enum class OtaWebCheckStatus {
    Disabled,
    Idle,
    Checking,
    Done,
  };

  struct OtaWebCheckSnapshot {
    OtaWebCheckStatus status = OtaWebCheckStatus::Disabled;
    bool available = false;
    std::string latestVersion;
    std::string message;
    int errorCode = 0;
  };

  static WebCompressedPayload getPokedexPluginPagePayload();
  static WebCompressedPayload getWallpaperPluginPagePayload();
  static WebCompressedPayload getAnkiPluginPagePayload();
  static OtaWebStartResult startOtaWebCheck();
  static OtaWebCheckSnapshot getOtaWebCheckSnapshot();

  /**
   * Scan/reload the user-font library and (if a USER_SD font is selected)
   * reload the active font family.  Returns metadata for building a JSON
   * response without the caller needing to know about UserFontManager.
   */
  static FontScanResult onFontScanRequested();
};

}  // namespace core
