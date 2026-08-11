#include "EbookSyncActivity.h"

#include <ArduinoJson.h>
#include <GfxRenderer.h>
#include <HTTPClient.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>

#include <cctype>

#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/StringUtils.h"

namespace {
constexpr const char* TAG = "EBOOK";
constexpr const char* LIST_TMP = "/x4_ebooks_list.tmp";
constexpr const char* DOWNLOAD_DIR = "/Download";
constexpr const char* NOTES_DIR = "/Notes";
constexpr size_t MAX_LOCAL_NAME_BYTES = 100;
constexpr int DOWNLOAD_PROGRESS_STEP_PERCENT = 5;
constexpr unsigned long DOWNLOAD_PROGRESS_MIN_UPDATE_MS = 5000;

std::string extensionOf(const std::string& path) {
  const auto slash = path.find_last_of('/');
  const auto dot = path.find_last_of('.');
  if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return "";
  return path.substr(dot);
}
}  // namespace

EbookSyncActivity::EbookSyncActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("EbookSync", renderer, mappedInput) {}

void EbookSyncActivity::onEnter() {
  Activity::onEnter();
  WiFi.mode(WIFI_STA);
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput),
                         [this](const ActivityResult& result) { onWifiSelectionComplete(!result.isCancelled); });
}

void EbookSyncActivity::onExit() {
  Activity::onExit();
  entries_.clear();
  Storage.remove(LIST_TMP);

  if (WiFi.getMode() != WIFI_MODE_NULL) {
    WiFi.disconnect(false);
    delay(30);
    silentRestart();
  }
}

void EbookSyncActivity::onWifiSelectionComplete(const bool success) {
  if (!success) {
    finish();
    return;
  }

  {
    RenderLock lock(*this);
    state_ = LOADING_LIST;
    statusMessage_ = tr(STR_EBOOK_SYNC_LOADING);
    errorMessage_.clear();
  }
  requestUpdateAndWait();

  if (!fetchAndParseList()) {
    RenderLock lock(*this);
    state_ = ERROR;
    return;
  }

  {
    RenderLock lock(*this);
    state_ = READY;
    selectedIndex_ = 0;
    statusMessage_.clear();
  }
  requestUpdate();
}

std::string EbookSyncActivity::urlEncode(const std::string& value) {
  static constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(HEX_DIGITS[c >> 4]);
      out.push_back(HEX_DIGITS[c & 0x0F]);
    }
  }
  return out;
}

std::string EbookSyncActivity::ensureExtensionPreserved(const std::string& filename, const std::string& originalPath) {
  const std::string ext = extensionOf(originalPath);
  std::string clean = StringUtils::sanitizeFilename(filename, MAX_LOCAL_NAME_BYTES);
  if (ext.empty()) return clean;

  if (clean.size() >= ext.size() && clean.compare(clean.size() - ext.size(), ext.size(), ext) == 0) {
    return clean;
  }

  const size_t baseBudget = MAX_LOCAL_NAME_BYTES > ext.size() ? MAX_LOCAL_NAME_BYTES - ext.size() : 1;
  clean = StringUtils::sanitizeFilename(filename, baseBudget);
  return clean + ext;
}

bool EbookSyncActivity::fetchAndParseList() {
  entries_.clear();
  Storage.remove(LIST_TMP);

  const auto result = HttpDownloader::downloadToFile(X4_EBOOKS_LIST_URL, LIST_TMP, nullptr);
  if (result != HttpDownloader::OK) {
    LOG_ERR(TAG, "Failed to fetch eBook list (%d)", static_cast<int>(result));
    errorMessage_ = tr(STR_EBOOK_SYNC_LIST_FAILED);
    Storage.remove(LIST_TMP);
    return false;
  }

  HalFile listFile;
  if (!Storage.openFileForRead(TAG, LIST_TMP, listFile)) {
    LOG_ERR(TAG, "Failed to open eBook list temp file");
    errorMessage_ = tr(STR_EBOOK_SYNC_LIST_FAILED);
    Storage.remove(LIST_TMP);
    return false;
  }

  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, listFile);
  listFile.close();
  Storage.remove(LIST_TMP);
  if (err || !doc.is<JsonArray>()) {
    LOG_ERR(TAG, "Invalid eBook list JSON: %s", err.c_str());
    errorMessage_ = tr(STR_EBOOK_SYNC_INVALID_LIST);
    return false;
  }

  JsonArray arr = doc.as<JsonArray>();
  entries_.reserve(arr.size());
  skippedExisting_ = 0;
  for (JsonObject obj : arr) {
    const char* path = obj["path"] | "";
    const char* filename = obj["filename"] | "";
    if (path[0] == '\0') continue;

    EbookEntry entry;
    entry.path = path;
    entry.filename = filename[0] != '\0' ? filename : path;
    entry.filename = ensureExtensionPreserved(entry.filename, entry.path);
    entry.localPath = std::string(DOWNLOAD_DIR) + "/" + entry.filename;
    entry.exists = Storage.exists(entry.localPath.c_str());
    if (entry.exists) skippedExisting_++;
    entries_.push_back(std::move(entry));
  }

  LOG_DBG(TAG, "Loaded %zu eBook entries", entries_.size());
  uploadPendingNotes();
  return true;
}

int EbookSyncActivity::listItemCount() const { return entries_.empty() ? 1 : static_cast<int>(entries_.size()) + 1; }

bool EbookSyncActivity::downloadEntry(EbookEntry& entry) {
  if (entry.exists) return true;
  if (!Storage.exists(DOWNLOAD_DIR) && !Storage.mkdir(DOWNLOAD_DIR)) {
    LOG_ERR(TAG, "Failed to create %s", DOWNLOAD_DIR);
    errorMessage_ = tr(STR_EBOOK_SYNC_DIR_FAILED);
    return false;
  }

  const std::string url = std::string(X4_EBOOKS_DOWNLOAD_URL) + "?file=" + urlEncode(entry.path);
  int lastRenderedPercent = -1;
  unsigned long lastProgressUpdateMs = 0;
  fileProgress_ = 0;
  fileTotal_ = 0;

  const auto result = HttpDownloader::downloadToFile(
      url, entry.localPath,
      [this, &lastRenderedPercent, &lastProgressUpdateMs](size_t downloaded, size_t total) {
        fileProgress_ = downloaded;
        fileTotal_ = total;
        mappedInput.update();
        if (mappedInput.isPressed(MappedInputManager::Button::Back) ||
            mappedInput.wasPressed(MappedInputManager::Button::Back)) {
          cancelRequested_ = true;
        }
        const int percent = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percent >= 100 || lastRenderedPercent < 0 ||
            percent >= lastRenderedPercent + DOWNLOAD_PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= DOWNLOAD_PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percent;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelRequested_);

  if (result == HttpDownloader::ABORTED) {
    return false;
  }
  if (result != HttpDownloader::OK) {
    LOG_ERR(TAG, "Download failed: %s (%d)", entry.path.c_str(), static_cast<int>(result));
    errorMessage_ = tr(STR_EBOOK_SYNC_DOWNLOAD_FAILED);
    return false;
  }

  entry.exists = true;
  return true;
}

bool EbookSyncActivity::uploadNoteFile(const std::string& path, const std::string& filename) {
  HalFile f;
  if (!Storage.openFileForRead(TAG, path, f)) return false;
  std::string body;
  body.reserve(std::min<size_t>(f.size(), 32 * 1024));
  while (f.available()) body.push_back(static_cast<char>(f.read()));
  f.close();

  NetworkClientSecure client;
  client.setInsecure();
  HTTPClient http;
  const std::string url =
      std::string(X4_NOTES_UPLOAD_URL) + "?folder=" + urlEncode("01_X4") + "&filename=" + urlEncode(filename);
  if (!http.begin(client, url.c_str())) return false;
  http.addHeader("Content-Type", "text/markdown; charset=utf-8");
  http.addHeader("X-X4-Note-Filename", filename.c_str());
  const int code = http.POST(reinterpret_cast<uint8_t*>(body.data()), body.size());
  http.end();
  return code >= 200 && code < 300;
}

bool EbookSyncActivity::uploadPendingNotes() {
  uploadedNotes_ = 0;
  deletedNotes_ = 0;
  if (!Storage.exists(NOTES_DIR)) return true;
  auto dir = Storage.open(NOTES_DIR);
  if (!dir || !dir.isDirectory()) return false;
  char name[160];
  bool ok = true;
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(name, sizeof(name));
    const bool isDir = f.isDirectory();
    f.close();
    if (isDir) continue;
    std::string filename{name};
    if (filename.size() < 3 || filename.substr(filename.size() - 3) != ".md") continue;
    const std::string path = std::string(NOTES_DIR) + "/" + filename;
    statusMessage_ = std::string(tr(STR_NOTE_SYNC_UPLOADING)) + " " + filename;
    requestUpdate(true);
    if (uploadNoteFile(path, filename)) {
      uploadedNotes_++;
      if (Storage.remove(path.c_str()))
        deletedNotes_++;
      else
        ok = false;
    } else {
      ok = false;
    }
  }
  dir.close();
  return ok;
}

void EbookSyncActivity::syncAllNew() {
  {
    RenderLock lock(*this);
    state_ = SYNCING;
    cancelRequested_ = false;
    currentIndex_ = 0;
    newDownloads_ = 0;
    fileProgress_ = 0;
    fileTotal_ = 0;
  }
  requestUpdateAndWait();

  for (size_t i = 0; i < entries_.size(); ++i) {
    currentIndex_ = i;
    if (entries_[i].exists) continue;
    if (!downloadEntry(entries_[i])) {
      RenderLock lock(*this);
      state_ = cancelRequested_ ? READY : ERROR;
      return;
    }
    newDownloads_++;
  }

  {
    RenderLock lock(*this);
    state_ = COMPLETE;
  }
  requestUpdate();
}

void EbookSyncActivity::loop() {
  if (state_ == READY) {
    auto activateSelected = [this] {
      if (entries_.empty() || selectedIndex_ == 0) {
        syncAllNew();
        return;
      }
      auto& entry = entries_[selectedIndex_ - 1];
      if (!entry.exists) {
        currentIndex_ = static_cast<size_t>(selectedIndex_ - 1);
        newDownloads_ = 0;
        {
          RenderLock lock(*this);
          state_ = SYNCING;
          cancelRequested_ = false;
        }
        requestUpdateAndWait();
        if (downloadEntry(entry)) {
          newDownloads_ = 1;
          RenderLock lock(*this);
          state_ = COMPLETE;
        } else {
          RenderLock lock(*this);
          state_ = cancelRequested_ ? READY : ERROR;
        }
        requestUpdate();
      }
    };

    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      finish();
      return;
    }

    const int listSize = listItemCount();
    const int pageItems = UITheme::getNumberOfItemsPerPage(renderer, true, false, true, false);
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up) {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down) {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
      return;
    }

    buttonNavigator_.onNextRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, listSize);
      requestUpdate();
    });
    buttonNavigator_.onPreviousRelease([this, listSize] {
      selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, listSize);
      requestUpdate();
    });
    buttonNavigator_.onNextContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::nextPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });
    buttonNavigator_.onPreviousContinuous([this, listSize, pageItems] {
      selectedIndex_ = ButtonNavigator::previousPageIndex(selectedIndex_, listSize, pageItems);
      requestUpdate();
    });

    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      activateSelected();
    }
  } else if (state_ == COMPLETE || state_ == ERROR) {
    int x = 0;
    int y = 0;
    if (mappedInput.wasPressed(MappedInputManager::Button::Back) ||
        mappedInput.wasPressed(MappedInputManager::Button::Confirm) || mappedInput.wasScreenTapped(x, y)) {
      finish();
    }
  }
}

void EbookSyncActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_EBOOK_SYNC));

  const auto lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const auto centerY = (pageHeight - lineHeight) / 2;

  if (state_ == LOADING_LIST || state_ == WIFI_SELECTION) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY,
                              statusMessage_.empty() ? tr(STR_EBOOK_SYNC_LOADING) : statusMessage_.c_str());
    GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");
  } else if (state_ == READY) {
    GUI.drawList(
        renderer,
        Rect{0, contentTop, pageWidth, pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing},
        listItemCount(), selectedIndex_,
        [this](int index) -> std::string {
          if (index == 0) return tr(STR_EBOOK_SYNC_ALL_NEW);
          return entries_[index - 1].filename;
        },
        [this](int index) -> std::string {
          if (index == 0) {
            return std::to_string(entries_.size() - skippedExisting_) + " " + tr(STR_EBOOK_SYNC_NEW_COUNT);
          }
          return entries_[index - 1].path;
        },
        nullptr,
        [this](int index) -> std::string {
          if (index == 0) return "";
          return entries_[index - 1].exists ? tr(STR_INSTALLED) : "";
        });
    GUI.drawButtonHints(renderer, tr(STR_BACK), tr(STR_DOWNLOAD), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  } else if (state_ == SYNCING) {
    const std::string title = currentIndex_ < entries_.size() ? entries_[currentIndex_].filename : tr(STR_DOWNLOADING);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, title.c_str());
    const int percent = fileTotal_ > 0 ? static_cast<int>(static_cast<uint64_t>(fileProgress_) * 100 / fileTotal_) : 0;
    GUI.drawProgressBar(renderer,
                        Rect{metrics.contentSidePadding, centerY + metrics.verticalSpacing,
                             pageWidth - metrics.contentSidePadding * 2, metrics.progressBarHeight},
                        percent, 100);
    GUI.drawButtonHints(renderer, tr(STR_CANCEL), "", "", "");
  } else if (state_ == COMPLETE) {
    const std::string done = std::string(tr(STR_EBOOK_SYNC_COMPLETE)) + " " + std::to_string(newDownloads_) + " " +
                             tr(STR_EBOOK_SYNC_NEW_COUNT);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY, done.c_str(), true, EpdFontFamily::BOLD);
    GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");
  } else if (state_ == ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, centerY - lineHeight, tr(STR_EBOOK_SYNC_FAILED), true,
                              EpdFontFamily::BOLD);
    renderer.drawCenteredText(UI_10_FONT_ID, centerY + metrics.verticalSpacing, errorMessage_.c_str());
    GUI.drawButtonHints(renderer, tr(STR_BACK), "", "", "");
  }

  renderer.displayBuffer();
}
