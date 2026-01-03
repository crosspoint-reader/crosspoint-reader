#include "OpdsBookBrowserActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr char OPDS_FEED_PATH[] = "/opds/navcatalog/4f6e6577657374?library_id=Calibre_Library";

// Prepend http:// if no protocol specified (server will redirect to https if needed)
std::string ensureProtocol(const std::string& url) {
  if (url.find("://") == std::string::npos) {
    return "http://" + url;
  }
  return url;
}
}  // namespace

void OpdsBookBrowserActivity::taskTrampoline(void* param) {
  auto* self = static_cast<OpdsBookBrowserActivity*>(param);
  self->displayTaskLoop();
}

void OpdsBookBrowserActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  state = BrowserState::LOADING;
  books.clear();
  selectorIndex = 0;
  errorMessage.clear();
  statusMessage = "Loading...";
  updateRequired = true;

  xTaskCreate(&OpdsBookBrowserActivity::taskTrampoline, "OpdsBookBrowserTask",
              4096,               // Stack size (larger for HTTP operations)
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );

  // Fetch books after setting up the display task
  fetchBooks();
}

void OpdsBookBrowserActivity::onExit() {
  Activity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  books.clear();
}

void OpdsBookBrowserActivity::loop() {
  // Handle error state - Confirm retries, Back goes home
  if (state == BrowserState::ERROR) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      state = BrowserState::LOADING;
      statusMessage = "Loading...";
      updateRequired = true;
      fetchBooks();
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  // Handle loading state - only Back goes home
  if (state == BrowserState::LOADING) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    }
    return;
  }

  // Handle downloading state - no input allowed
  if (state == BrowserState::DOWNLOADING) {
    return;
  }

  // Handle book list state
  if (state == BrowserState::BOOK_LIST) {
    const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Left);
    const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                              mappedInput.wasReleased(MappedInputManager::Button::Right);
    const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (!books.empty()) {
        downloadBook(books[selectorIndex]);
      }
    } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      onGoHome();
    } else if (prevReleased && !books.empty()) {
      if (skipPage) {
        selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + books.size()) % books.size();
      } else {
        selectorIndex = (selectorIndex + books.size() - 1) % books.size();
      }
      updateRequired = true;
    } else if (nextReleased && !books.empty()) {
      if (skipPage) {
        selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % books.size();
      } else {
        selectorIndex = (selectorIndex + 1) % books.size();
      }
      updateRequired = true;
    }
  }
}

void OpdsBookBrowserActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void OpdsBookBrowserActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Browse Books", true, EpdFontFamily::BOLD);

  if (state == BrowserState::LOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, statusMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::ERROR) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Error:");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, errorMessage.c_str());
    const auto labels = mappedInput.mapLabels("« Back", "Retry", "", "");
    renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == BrowserState::DOWNLOADING) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 - 20, "Downloading...");
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 10, statusMessage.c_str());
    if (downloadTotal > 0) {
      const int percent = (downloadProgress * 100) / downloadTotal;
      char progressText[32];
      snprintf(progressText, sizeof(progressText), "%d%%", percent);
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 40, progressText);
    }
    renderer.displayBuffer();
    return;
  }

  // Book list state
  const auto labels = mappedInput.mapLabels("« Back", "Download", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (books.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2, "No books found");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);

  for (size_t i = pageStartIndex; i < books.size() && i < static_cast<size_t>(pageStartIndex + PAGE_ITEMS); i++) {
    // Format: "Title - Author" or just "Title" if no author
    std::string displayText = books[i].title;
    if (!books[i].author.empty()) {
      displayText += " - " + books[i].author;
    }

    auto item = renderer.truncatedText(UI_10_FONT_ID, displayText.c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != static_cast<size_t>(selectorIndex));
  }

  renderer.displayBuffer();
}

void OpdsBookBrowserActivity::fetchBooks() {
  const char* serverUrl = SETTINGS.opdsServerUrl;
  if (strlen(serverUrl) == 0) {
    state = BrowserState::ERROR;
    errorMessage = "No server URL configured";
    updateRequired = true;
    return;
  }

  std::string url = ensureProtocol(serverUrl) + OPDS_FEED_PATH;
  Serial.printf("[%lu] [OPDS] Fetching: %s\n", millis(), url.c_str());

  std::string content;
  if (!HttpDownloader::fetchUrl(url, content)) {
    state = BrowserState::ERROR;
    errorMessage = "Failed to fetch feed";
    updateRequired = true;
    return;
  }

  OpdsParser parser;
  if (!parser.parse(content.c_str(), content.size())) {
    state = BrowserState::ERROR;
    errorMessage = "Failed to parse feed";
    updateRequired = true;
    return;
  }

  books = parser.getBooks();
  selectorIndex = 0;

  if (books.empty()) {
    state = BrowserState::ERROR;
    errorMessage = "No books found";
    updateRequired = true;
    return;
  }

  state = BrowserState::BOOK_LIST;
  updateRequired = true;
}

void OpdsBookBrowserActivity::downloadBook(const OpdsBook& book) {
  state = BrowserState::DOWNLOADING;
  statusMessage = book.title;
  downloadProgress = 0;
  downloadTotal = 0;
  updateRequired = true;

  // Build full download URL
  std::string downloadUrl = ensureProtocol(SETTINGS.opdsServerUrl) + book.epubUrl;

  // Create sanitized filename
  std::string filename = "/" + sanitizeFilename(book.title) + ".epub";

  Serial.printf("[%lu] [OPDS] Downloading: %s -> %s\n", millis(), downloadUrl.c_str(), filename.c_str());

  const auto result = HttpDownloader::downloadToFile(
      downloadUrl, filename, [this](const size_t downloaded, const size_t total) {
        downloadProgress = downloaded;
        downloadTotal = total;
        updateRequired = true;
      });

  if (result == HttpDownloader::OK) {
    Serial.printf("[%lu] [OPDS] Download complete: %s\n", millis(), filename.c_str());
    state = BrowserState::BOOK_LIST;
    updateRequired = true;
  } else {
    state = BrowserState::ERROR;
    errorMessage = "Download failed";
    updateRequired = true;
  }
}

std::string OpdsBookBrowserActivity::sanitizeFilename(const std::string& title) const {
  std::string result;
  result.reserve(title.size());

  for (char c : title) {
    // Replace invalid filename characters with underscore
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      result += '_';
    } else if (c >= 32 && c < 127) {
      // Keep printable ASCII characters
      result += c;
    }
    // Skip non-printable characters
  }

  // Trim leading/trailing spaces and dots
  size_t start = result.find_first_not_of(" .");
  if (start == std::string::npos) {
    return "book";  // Fallback if title is all invalid characters
  }
  size_t end = result.find_last_not_of(" .");
  result = result.substr(start, end - start + 1);

  // Limit filename length (SD card FAT32 has 255 char limit, but let's be safe)
  if (result.length() > 100) {
    result = result.substr(0, 100);
  }

  return result.empty() ? "book" : result;
}
