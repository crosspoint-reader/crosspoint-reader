#include "WallabagActivity.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <WallabagClient.h>
#include <WallabagCredentialStore.h>
#include <WiFi.h>

#include <algorithm>
#include <vector>

#include "MappedInputManager.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int MENU_ITEM_COUNT = 2;

// Sanitize article title for use in a filename (replaces unsafe chars, truncates)
std::string sanitizeArticleTitle(const std::string& title, size_t maxLen = 60) {
  std::string result;
  result.reserve(title.size());
  for (char c : title) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      result += '_';
    } else {
      result += c;
    }
  }
  if (result.size() > maxLen) {
    // Walk back to a valid UTF-8 character boundary (skip continuation bytes 0x80–0xBF)
    size_t cut = maxLen;
    while (cut > 0 && (static_cast<unsigned char>(result[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    result = result.substr(0, cut);
  }
  return result;
}

// Build article filename: /Articles/[w-id_<id>] <title>.epub
std::string articleFilename(int id, const std::string& title) {
  return "/Articles/[w-id_" + std::to_string(id) + "] " + sanitizeArticleTitle(title) + ".epub";
}

// Check if an article with the given id already exists in /Articles/
bool articleExists(int id) {
  const std::string prefix = "[w-id_" + std::to_string(id) + "]";
  std::vector<String> files = Storage.listFiles("/Articles/", 500);
  for (const auto& f : files) {
    if (f.startsWith(prefix.c_str())) {
      return true;
    }
  }
  return false;
}

// Returns filenames (just the filename, not full path) of existing wallabag articles
std::vector<std::string> listExistingArticles() {
  std::vector<std::string> result;
  std::vector<String> files = Storage.listFiles("/Articles/", 500);
  for (const auto& f : files) {
    // Only count files matching the wallabag naming pattern
    if (f.startsWith("[w-id_") && f.endsWith(".epub")) {
      result.push_back(f.c_str());
    }
  }
  return result;
}

// Extract the numeric Wallabag article ID from a filename like "[w-id_42] Title.epub".
// Returns -1 if the pattern is not found.
int extractWallabagId(const std::string& filename) {
  const std::string prefix = "[w-id_";
  const size_t start = filename.find(prefix);
  if (start == std::string::npos) return -1;
  const size_t numStart = start + prefix.size();
  const size_t numEnd = filename.find(']', numStart);
  if (numEnd == std::string::npos) return -1;
  try {
    return std::stoi(filename.substr(numStart, numEnd - numStart));
  } catch (...) {
    return -1;
  }
}

// Delete oldest wallabag articles (lowest ID first) to keep total within limit.
void enforceArticleLimit(int limit, int newCount) {
  if (limit <= 0) return;

  std::vector<std::string> existing = listExistingArticles();
  int total = static_cast<int>(existing.size()) + newCount;
  if (total <= limit) return;

  // Sort numerically by article ID so the oldest (lowest ID) are deleted first.
  // A lexicographic sort would wrongly order e.g. [w-id_10] before [w-id_2].
  std::sort(existing.begin(), existing.end(), [](const std::string& a, const std::string& b) {
    return extractWallabagId(a) < extractWallabagId(b);
  });

  int toDelete = total - limit;
  for (int i = 0; i < toDelete && i < static_cast<int>(existing.size()); i++) {
    const std::string fullPath = "/Articles/" + existing[i];
    LOG_DBG("WBG", "Removing old article: %s", fullPath.c_str());
    Storage.remove(fullPath.c_str());
  }
}
}  // namespace

void WallabagActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  state = State::MENU;
  menuIndex = 0;
  errorMessage.clear();
  syncPending = false;
  requestUpdate();
}

void WallabagActivity::onExit() {
  ActivityWithSubactivity::onExit();
  WiFi.mode(WIFI_OFF);
}

void WallabagActivity::loop() {
  if (state == State::WIFI_SELECTION) {
    ActivityWithSubactivity::loop();
    return;
  }

  if (state == State::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      // Retry: re-check WiFi and go back to menu
      state = State::CHECK_WIFI;
      statusMessage = tr(STR_CHECKING_WIFI);
      requestUpdate();
      checkAndConnectWifi();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::CHECK_WIFI) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  if (state == State::SYNCING) {
    if (syncPending) {
      syncPending = false;
      runSync();
    }
    return;
  }

  if (state == State::DONE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::MENU;
      requestUpdate();
    }
    return;
  }

  if (state == State::MENU) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
      return;
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (menuIndex == 0) {
        // Go to Articles Folder
        onGoToArticles();
      } else {
        // Download New Articles — check WiFi first
        state = State::CHECK_WIFI;
        statusMessage = tr(STR_CHECKING_WIFI);
        requestUpdate();
        checkAndConnectWifi();
      }
      return;
    }

    buttonNavigator.onNextRelease([this] {
      menuIndex = (menuIndex + 1) % MENU_ITEM_COUNT;
      requestUpdate();
    });

    buttonNavigator.onPreviousRelease([this] {
      menuIndex = (menuIndex + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
      requestUpdate();
    });
  }
}

void WallabagActivity::render(Activity::RenderLock&&) {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, tr(STR_WALLABAG_BROWSER), true, EpdFontFamily::BOLD);

  if (state == State::CHECK_WIFI || state == State::WIFI_SELECTION) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, tr(STR_ERROR_MSG));
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_RETRY), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == State::SYNCING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, statusMessage.c_str());
    if (totalToDownload > 0) {
      char progressMsg[64];
      snprintf(progressMsg, sizeof(progressMsg), "%d / %d", currentArticleNum, totalToDownload);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, progressMsg);
    }
    renderer.displayBuffer();
    return;
  }

  if (state == State::DONE) {
    char msg[64];
    if (downloadedCount == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, tr(STR_WALLABAG_NO_ARTICLES));
    } else {
      snprintf(msg, sizeof(msg), tr(STR_WALLABAG_ARTICLES_DOWNLOADED), downloadedCount);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, msg);
    }
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 30, tr(STR_WALLABAG_SYNC_COMPLETE));
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OK_BUTTON), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // MENU state
  const char* items[MENU_ITEM_COUNT] = {tr(STR_WALLABAG_GO_TO_FOLDER), tr(STR_WALLABAG_DOWNLOAD)};
  auto metrics = UITheme::getInstance().getMetrics();
  const int menuTop = 50;
  const int menuHeight = pageHeight - menuTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  GUI.drawButtonMenu(renderer, Rect{0, menuTop, pageWidth, menuHeight}, MENU_ITEM_COUNT, menuIndex,
                     [&items](int i) { return std::string(items[i]); },
                     [](int i) -> UIIcon { return (i == 0) ? Folder : Sync; });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}

void WallabagActivity::checkAndConnectWifi() {
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    state = State::SYNCING;
    syncPending = true;
    statusMessage = tr(STR_WALLABAG_SYNCING);
    downloadedCount = 0;
    totalToDownload = 0;
    currentArticleNum = 0;
    requestUpdate();
    return;
  }
  launchWifiSelection();
}

void WallabagActivity::launchWifiSelection() {
  state = State::WIFI_SELECTION;
  requestUpdate();

  enterNewActivity(new WifiSelectionActivity(renderer, mappedInput,
                                             [this](bool connected) { onWifiSelectionComplete(connected); }));
}

void WallabagActivity::onWifiSelectionComplete(bool connected) {
  exitActivity();

  if (connected) {
    state = State::SYNCING;
    syncPending = true;
    statusMessage = tr(STR_WALLABAG_SYNCING);
    downloadedCount = 0;
    totalToDownload = 0;
    currentArticleNum = 0;
    requestUpdate();
  } else {
    WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    state = State::ERROR;
    errorMessage = tr(STR_WIFI_CONN_FAILED);
    requestUpdate();
  }
}

void WallabagActivity::runSync() {
  // Step 1: Authenticate if needed
  if (!WALLABAG_STORE.isTokenValid()) {
    statusMessage = tr(STR_AUTHENTICATING);
    requestUpdate();
    const WallabagClient::Error authErr = WallabagClient::authenticate();
    if (authErr != WallabagClient::OK) {
      state = State::ERROR;
      errorMessage = WallabagClient::errorString(authErr);
      requestUpdate();
      return;
    }
  }

  // Step 2: Fetch article list
  statusMessage = tr(STR_WALLABAG_SYNCING);
  requestUpdate();

  std::vector<WallabagArticle> articles;
  const int limit = WALLABAG_STORE.getArticleLimit();
  const WallabagClient::Error fetchErr = WallabagClient::fetchArticles(articles, limit);
  if (fetchErr != WallabagClient::OK) {
    state = State::ERROR;
    errorMessage = WallabagClient::errorString(fetchErr);
    requestUpdate();
    return;
  }

  // Ensure /Articles directory exists
  Storage.mkdir("/Articles");

  // Step 3: Find articles not yet downloaded
  std::vector<WallabagArticle> toDownload;
  for (const auto& article : articles) {
    if (!articleExists(article.id)) {
      toDownload.push_back(article);
    }
  }

  // Step 4: Enforce article limit - delete oldest articles to make room
  enforceArticleLimit(limit > 0 ? limit : 0, static_cast<int>(toDownload.size()));

  // Step 5: Download each new article
  totalToDownload = static_cast<int>(toDownload.size());
  downloadedCount = 0;
  currentArticleNum = 0;

  for (const auto& article : toDownload) {
    currentArticleNum++;
    const std::string destPath = articleFilename(article.id, article.title);
    LOG_DBG("WBG", "Downloading article %d (%d/%d): %s", article.id, currentArticleNum, totalToDownload,
            article.title.c_str());

    char msg[64];
    snprintf(msg, sizeof(msg), tr(STR_WALLABAG_DOWNLOADING), currentArticleNum, totalToDownload);
    statusMessage = msg;
    requestUpdate();

    const WallabagClient::Error dlErr = WallabagClient::downloadArticle(
        article.id, destPath, [this](size_t /*downloaded*/, size_t /*total*/) { requestUpdate(); });

    if (dlErr == WallabagClient::OK) {
      // Invalidate any stale epub cache for this file
      Epub epub(destPath, "/.crosspoint");
      epub.clearCache();
      downloadedCount++;
    } else {
      LOG_ERR("WBG", "Failed to download article %d: %s", article.id, WallabagClient::errorString(dlErr));
      // Continue with remaining articles rather than aborting
    }
  }

  state = State::DONE;
  requestUpdate();
}
