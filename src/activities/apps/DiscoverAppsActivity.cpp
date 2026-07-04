#include "DiscoverAppsActivity.h"

#include <AppDateTimeFormat.h>
#include <AppListLabels.h>
#include <AppPathSanitizer.h>
#include <AppRegistry.h>
#include <AppStorePaths.h>
#include <AppStoreManifest.h>
#include <AppStoreManifestMeta.h>
#include <AppStoreManifestTypes.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <WiFi.h>
#include <ZipFile.h>

#include <algorithm>
#include <cstring>

#include <cstdio>
#include <cstdlib>
#include <string_view>

#include "ApplicationsMenuActivity.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {

bool ensureParentDirectoryExists(const char* filePath) {
  const char* lastSlash = strrchr(filePath, '/');
  if (lastSlash == nullptr || lastSlash == filePath) {
    return true;
  }
  char dirPath[128];
  const size_t len = static_cast<size_t>(lastSlash - filePath);
  if (len >= sizeof(dirPath)) {
    return false;
  }
  memcpy(dirPath, filePath, len);
  dirPath[len] = '\0';
  if (Storage.exists(dirPath)) {
    return true;
  }
  return Storage.mkdir(dirPath, true);
}

bool extractZipEntry(ZipFile& zip, const char* zipEntry, const char* destPath) {
  if (!ensureParentDirectoryExists(destPath)) {
    return false;
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("APPS", destPath, outFile)) {
    LOG_ERR("APPS", "Install failed: could not open %s for write", destPath);
    return false;
  }

  const bool ok = zip.readFileToStream(zipEntry, outFile, 1024);
  outFile.close();
  if (!ok) {
    Storage.remove(destPath);
  }
  return ok;
}

static constexpr size_t kMaxBundleFiles = 48;
static constexpr size_t kMaxBundleEntryLen = 96;

struct BundleFileEntry {
  char zipPath[kMaxBundleEntryLen];
  char destPath[128];
};

}  // namespace

DiscoverAppsActivity::DiscoverAppsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("DiscoverApps", renderer, mappedInput) {}

void DiscoverAppsActivity::sortEntries() {
  std::sort(entries_.begin(), entries_.end(), [this](const AppCatalogEntry& a, const AppCatalogEntry& b) {
    const auto* installedA = findInstalledEntry(a.id);
    const auto* installedB = findInstalledEntry(b.id);
    const std::string dateA = installedA != nullptr ? installedA->installedAt : "";
    const std::string dateB = installedB != nullptr ? installedB->installedAt : "";

    if (AppDateTimeFormat::isNewerInstalledAt(dateA, dateB)) {
      return true;
    }
    if (AppDateTimeFormat::isNewerInstalledAt(dateB, dateA)) {
      return false;
    }
    return a.name < b.name;
  });
}

void DiscoverAppsActivity::loadCatalog() {
  const bool loaded = APP_STORE_CATALOG.load();
  entries_ = APP_STORE_CATALOG.getEntries();
  LOG_DBG("APPS", "Discover catalog load=%s entries=%u", loaded ? "ok" : "fail",
          static_cast<unsigned>(entries_.size()));
  for (const auto& entry : entries_) {
    LOG_DBG("APPS", "Discover app id=%s name=%s version=%s", entry.id.c_str(), entry.name.c_str(), entry.version.c_str());
  }

  APP_REGISTRY.loadFromFile();
  installedById_.clear();
  for (const auto& entry : APP_REGISTRY.getEntries()) {
    installedById_[entry.id] = entry;
  }

  hasManifestMeta_ = AppStoreManifestMeta::readMeta(manifestMeta_);
  if (!hasManifestMeta_ && APP_STORE_CATALOG.getSource() == AppCatalogSource::Remote) {
    manifestMeta_.appCount = static_cast<uint32_t>(entries_.size());
    manifestMeta_.fetchedAt = AppDateTimeFormat::formatNowIso8601Utc();
    hasManifestMeta_ = !manifestMeta_.fetchedAt.empty();
  }

  sortEntries();
  selectedIndex = 0;
  requestUpdate();
}

const AppRegistryEntry* DiscoverAppsActivity::findInstalledEntry(const std::string& appId) const {
  const auto it = installedById_.find(appId);
  if (it == installedById_.end()) {
    return nullptr;
  }
  return &it->second;
}

std::string DiscoverAppsActivity::formatInstalledSubtitle(const std::string& dateDisplay) const {
  if (dateDisplay.empty()) {
    return "";
  }
  char buf[64];
  snprintf(buf, sizeof(buf), tr(STR_APPS_INSTALLED_ON), dateDisplay.c_str());
  return buf;
}

std::string DiscoverAppsActivity::formatAvailableCountLabel() const {
  if (hasManifestMeta_) {
    char buf[32];
    snprintf(buf, sizeof(buf), tr(STR_APPS_AVAILABLE_COUNT), static_cast<int>(manifestMeta_.appCount));
    return buf;
  }
  return tr(STR_APPS_STORE_COUNT_UNKNOWN);
}

std::string DiscoverAppsActivity::formatInstalledCountLabel() const {
  char buf[32];
  snprintf(buf, sizeof(buf), tr(STR_APPS_INSTALLED_COUNT), static_cast<int>(installedById_.size()));
  return buf;
}

void DiscoverAppsActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    LOG_ERR("APPS", "Discover WiFi connection failed, returning to Applications");
    activityManager.replaceActivity(std::make_unique<ApplicationsMenuActivity>(renderer, mappedInput));
    return;
  }

  LOG_DBG("APPS", "Discover WiFi connected, loading catalog");
  loadCatalog();
}

void DiscoverAppsActivity::onEnter() {
  Activity::onEnter();
  LOG_DBG("APPS", "Discover activity entered");
  if (hasAttemptedLoad_) {
    return;
  }
  hasAttemptedLoad_ = true;

  if (WiFi.status() == WL_CONNECTED) {
    LOG_DBG("APPS", "Discover WiFi already connected");
    loadCatalog();
    return;
  }

  LOG_DBG("APPS", "Discover launching WifiSelectionActivity...");
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void DiscoverAppsActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.replaceActivity(std::make_unique<ApplicationsMenuActivity>(renderer, mappedInput));
    return;
  }

  if (entries_.empty()) {
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (installSelectedEntry()) {
      APP_REGISTRY.loadFromFile();
      installedById_.clear();
      for (const auto& entry : APP_REGISTRY.getEntries()) {
        installedById_[entry.id] = entry;
      }
      sortEntries();
      requestUpdate();
    }
    return;
  }

  const int itemCount = static_cast<int>(entries_.size());
  buttonNavigator.onNextRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, itemCount);
    requestUpdate();
  });

  buttonNavigator.onPreviousRelease([this, itemCount] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, itemCount);
    requestUpdate();
  });
}

void DiscoverAppsActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const bool use12Hour = SETTINGS.clockFormat == 1;
  const uint8_t utcOffset = SETTINGS.clockUtcOffsetQ;

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DISCOVER_APPS));
  GUI.drawSubHeader(renderer, Rect{0, metrics.topPadding + metrics.headerHeight, pageWidth, metrics.tabBarHeight},
                    formatAvailableCountLabel().c_str(), formatInstalledCountLabel().c_str());

  const int contentTop =
      metrics.topPadding + metrics.headerHeight + metrics.tabBarHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing * 2;
  const int itemCount = static_cast<int>(entries_.size());

  if (itemCount == 0) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_NO_DISCOVER_APPS));
  } else {
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [this](int index) { return entries_[static_cast<size_t>(index)].name; },
        [this, utcOffset, use12Hour](int index) {
          const auto& entry = entries_[static_cast<size_t>(index)];
          const auto* installed = findInstalledEntry(entry.id);
          const auto labels = AppListLabels::buildDiscoverRowLabels(entry, installed, utcOffset, use12Hour);
          return formatInstalledSubtitle(labels.subtitle);
        },
        nullptr,
        [this, utcOffset, use12Hour](int index) {
          const auto& entry = entries_[static_cast<size_t>(index)];
          const auto* installed = findInstalledEntry(entry.id);
          return AppListLabels::buildDiscoverRowLabels(entry, installed, utcOffset, use12Hour).rowValue;
        },
        true);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

bool DiscoverAppsActivity::installSelectedEntry() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(entries_.size())) {
    return false;
  }

  const auto& entry = entries_[static_cast<size_t>(selectedIndex)];
  if (!AppPathSanitizer::isValidAppId(entry.id)) {
    LOG_ERR("APPS", "Install rejected: invalid app id %s", entry.id.c_str());
    return false;
  }
  if (entry.bundleUrl.empty()) {
    LOG_ERR("APPS", "Install rejected: app %s has empty bundle URL", entry.id.c_str());
    return false;
  }

  Storage.ensureDirectoryExists(AppStorePaths::kTmpDir);
  Storage.ensureDirectoryExists(AppStorePaths::kAppsRoot);

  char tmpZipPath[96];
  snprintf(tmpZipPath, sizeof(tmpZipPath), "%s/%s.cpapp", AppStorePaths::kTmpDir, entry.id.c_str());
  char appDirPath[96];
  snprintf(appDirPath, sizeof(appDirPath), "%s/%s", AppStorePaths::kAppsRoot, entry.id.c_str());

  LOG_DBG("APPS", "Installing app id=%s from %s", entry.id.c_str(), entry.bundleUrl.c_str());
  const auto downloadRes = HttpDownloader::downloadToFile(entry.bundleUrl, tmpZipPath, nullptr);
  if (downloadRes != HttpDownloader::OK) {
    LOG_ERR("APPS", "Install failed: download error %d", static_cast<int>(downloadRes));
    Storage.remove(tmpZipPath);
    return false;
  }

  if (Storage.exists(appDirPath)) {
    if (!Storage.removeDir(appDirPath)) {
      LOG_ERR("APPS", "Install failed: could not remove existing app dir %s", appDirPath);
      Storage.remove(tmpZipPath);
      return false;
    }
  }
  Storage.ensureDirectoryExists(appDirPath);

  ZipFile zip(tmpZipPath);
  bool extractOk = true;
  bool hasMainLua = false;
  bool hasManifest = false;
  auto bundleFiles = makeUniqueNoThrow<BundleFileEntry[]>(kMaxBundleFiles);
  if (!bundleFiles) {
    LOG_ERR("APPS", "Install failed: OOM bundle file table");
    Storage.remove(tmpZipPath);
    return false;
  }
  size_t bundleFileCount = 0;

  zip.enumerateFilePaths([&](const std::string_view entryPath) {
    if (!extractOk) {
      return;
    }

    if (entryPath.empty() || entryPath.back() == '/') {
      return;
    }

    const auto sanitized = AppPathSanitizer::sanitizeRelativePath(entryPath);
    if (!sanitized.has_value()) {
      LOG_ERR("APPS", "Install rejected: unsafe bundle path %.*s", static_cast<int>(entryPath.size()), entryPath.data());
      extractOk = false;
      return;
    }

    if (sanitized->normalized == "main.lua") {
      hasMainLua = true;
    } else if (sanitized->normalized == "manifest.json") {
      hasManifest = true;
    }

    if (bundleFileCount >= kMaxBundleFiles) {
      LOG_ERR("APPS", "Install failed: bundle has too many files");
      extractOk = false;
      return;
    }

    if (entryPath.size() >= kMaxBundleEntryLen) {
      LOG_ERR("APPS", "Install failed: bundle entry path too long");
      extractOk = false;
      return;
    }

    BundleFileEntry& fileEntry = bundleFiles[bundleFileCount];
    memcpy(fileEntry.zipPath, entryPath.data(), entryPath.size());
    fileEntry.zipPath[entryPath.size()] = '\0';

    const int destWritten =
        snprintf(fileEntry.destPath, sizeof(fileEntry.destPath), "%s/%s", appDirPath, sanitized->normalized.c_str());
    if (destWritten <= 0 || static_cast<size_t>(destWritten) >= sizeof(fileEntry.destPath)) {
      LOG_ERR("APPS", "Install failed: path too long for %s", sanitized->normalized.c_str());
      extractOk = false;
      return;
    }

    bundleFileCount++;
  });

  for (size_t i = 0; extractOk && i < bundleFileCount; i++) {
    const BundleFileEntry& fileEntry = bundleFiles[i];
    if (!extractZipEntry(zip, fileEntry.zipPath, fileEntry.destPath)) {
      LOG_ERR("APPS", "Install failed: could not extract %s", fileEntry.zipPath);
      extractOk = false;
      break;
    }
  }

  Storage.remove(tmpZipPath);

  if (!extractOk || !hasMainLua || !hasManifest) {
    if (!hasMainLua || !hasManifest) {
      LOG_ERR("APPS", "Install failed: bundle missing required files");
    }
    return false;
  }

  AppRegistryEntry registryEntry;
  registryEntry.id = entry.id;
  registryEntry.name = entry.name;
  registryEntry.version = entry.version;
  registryEntry.installedAt = AppDateTimeFormat::formatNowIso8601Utc();
  if (!APP_REGISTRY.upsertEntry(registryEntry)) {
    LOG_ERR("APPS", "Install failed: could not update registry for %s", entry.id.c_str());
    return false;
  }

  LOG_INF("APPS", "Installed app id=%s version=%s", entry.id.c_str(), entry.version.c_str());
  return true;
}
