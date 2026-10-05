#include "PluginHubInstallActivity.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>

#include "activities/util/ConfirmationActivity.h"
#include "components/CatalogScreens.h"
#include "util/PluginLocations.h"

namespace {
constexpr const char* PLUGIN_HUB_BASE_URL =
    "https://raw.githubusercontent.com/jadehawk/PluginHub.crosspoint-plugin/stable/";
constexpr const char* PLUGIN_HUB_DIR = "/.crosspoint/plugins/pluginhub";
constexpr const char* PLUGIN_HUB_INSTALL_MARKER = "/.crosspoint/plugins/pluginhub/.installing";
constexpr const char* PLUGIN_HUB_FILES[] = {"manifest.json", "device.json", "plugin.js", "README.md"};

std::string pluginHubPath(const char* filename) { return std::string(PLUGIN_HUB_DIR) + "/" + filename; }
}  // namespace

PluginHubInstallActivity::PluginHubInstallActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : CatalogActivity("PluginHubInstall", renderer, mappedInput) {}

bool PluginHubInstallActivity::isInstalled() {
  const std::string dir = PluginLocations::findPluginDir("pluginhub");
  if (dir.empty() || Storage.exists(PLUGIN_HUB_INSTALL_MARKER)) return false;
  return std::all_of(std::begin(PLUGIN_HUB_FILES), std::end(PLUGIN_HUB_FILES),
                     [&dir](const char* filename) { return Storage.exists((dir + "/" + filename).c_str()); });
}

void PluginHubInstallActivity::onEnter() {
  CatalogActivity::onEnter();

  auto confirmation = makeUniqueNoThrow<ConfirmationActivity>(renderer, mappedInput, tr(STR_INSTALL_PLUGIN_HUB),
                                                              tr(STR_PLUGIN_HUB_INSTALL_DESCRIPTION));
  if (!confirmation) {
    LOG_ERR("PHUB", "OOM: install confirmation");
    fail(StrId::STR_MEMORY_ERROR);
    return;
  }

  startActivityForResult(std::move(confirmation), [this](const ActivityResult& result) {
    if (result.isCancelled) {
      finish();
      return;
    }
    state = State::CHECK_WIFI;
    statusMessage = tr(STR_CHECKING_WIFI);
    requestUpdate();
    checkAndConnectWifi();
  });
}

void PluginHubInstallActivity::startBrowse() {
  beginDownload(tr(STR_INSTALL_PLUGIN_HUB));
  requestUpdateAndWait();
  finishDownload(installPluginHub());
}

HttpDownloader::DownloadError PluginHubInstallActivity::installPluginHub() {
  if ((!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) ||
      (!Storage.exists(PluginLocations::ROOTS[0]) && !Storage.mkdir(PluginLocations::ROOTS[0]))) {
    LOG_ERR("PHUB", "Unable to create plugin root");
    return HttpDownloader::FILE_ERROR;
  }

  const bool installDirExisted = Storage.exists(PLUGIN_HUB_DIR);
  if (!installDirExisted && !Storage.mkdir(PLUGIN_HUB_DIR)) {
    LOG_ERR("PHUB", "Unable to create %s", PLUGIN_HUB_DIR);
    return HttpDownloader::FILE_ERROR;
  }

  cleanupStagedFiles();

  // Keep only one URL/destination pair alive at a time. The four runtime files
  // are small, and staging each as .new means a failed transfer never replaces
  // an existing Plugin Hub file.
  for (const char* filename : PLUGIN_HUB_FILES) {
    const std::string dest = pluginHubPath(filename);
    const std::string staged = dest + ".new";
    const std::string url = std::string(PLUGIN_HUB_BASE_URL) + filename;
    const auto result = downloadFile(url, staged);
    if (result != HttpDownloader::OK) {
      cleanupStagedFiles();
      if (!installDirExisted) Storage.rmdir(PLUGIN_HUB_DIR);
      return result;
    }
  }

  // Mark the publish phase so a failed/crashed multi-file swap cannot make a
  // partial bundle appear installed. A retry replaces every runtime file.
  if (!Storage.writeFile(PLUGIN_HUB_INSTALL_MARKER, String("installing"))) {
    cleanupStagedFiles();
    return HttpDownloader::FILE_ERROR;
  }

  // Publish only after all four files arrived. This mirrors the native plugin
  // catalog bundle installer and makes network failures safe to retry.
  for (const char* filename : PLUGIN_HUB_FILES) {
    const std::string dest = pluginHubPath(filename);
    const std::string staged = dest + ".new";
    if (!Storage.replaceFile(staged.c_str(), dest.c_str())) {
      LOG_ERR("PHUB", "Install swap failed: %s", dest.c_str());
      cleanupStagedFiles();
      return HttpDownloader::FILE_ERROR;
    }
  }

  if (!Storage.remove(PLUGIN_HUB_INSTALL_MARKER)) {
    LOG_ERR("PHUB", "Unable to clear install marker");
    return HttpDownloader::FILE_ERROR;
  }
  return HttpDownloader::OK;
}

void PluginHubInstallActivity::cleanupStagedFiles() {
  for (const char* filename : PLUGIN_HUB_FILES) {
    const std::string staged = pluginHubPath(filename) + ".new";
    Storage.remove(staged.c_str());
  }
}

void PluginHubInstallActivity::downloadFinished(const bool cancelled) {
  if (cancelled) {
    finish();
    return;
  }
  state = State::DONE;
  requestUpdate();
}

bool PluginHubInstallActivity::handleCustomInput() {
  if (CatalogActivity::handleCustomInput()) return true;
  if (state != State::DONE) return false;

  int x = 0, y = 0;
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      mappedInput.wasReleased(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
    finish();
  }
  return true;
}

void PluginHubInstallActivity::onBackButton() { finish(); }

void PluginHubInstallActivity::buildScreen(UiScreen& screen) {
  screenHeader(screen, tr(STR_INSTALL_PLUGIN_HUB));
  if (buildStatusScreen(screen, true, true)) return;

  if (state == State::DONE) {
    catalogCenteredBlock(screen, {{tr(STR_PLUGIN_HUB_INSTALLED), true}, {tr(STR_PLUGIN_HUB_INSTALLED_HINT)}});
  }
}
