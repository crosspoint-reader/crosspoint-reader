#include "SteganographyActivity.h"

#include <HalStorage.h>
#include <Logging.h>

#include <cstring>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void SteganographyActivity::onEnter() {
  Activity::onEnter();
  state = MODE_SELECT;
  mode = EMBED;
  fileCount = 0;
  fileIndex = 0;
  messageLen = 0;
  messageBuffer[0] = '\0';
  scrollOffset = 0;
  embedSuccess = false;
  extractFound = false;
  requestUpdate();
}

void SteganographyActivity::onExit() { Activity::onExit(); }

// ---------------------------------------------------------------------------
// File listing
// ---------------------------------------------------------------------------

void SteganographyActivity::loadFileList() {
  fileCount = 0;
  fileIndex = 0;

  HalFile dir = Storage.open("/biscuit/drawings");
  if (!dir || !dir.isDirectory()) return;

  HalFile entry;
  while ((entry = dir.openNextFile()) && fileCount < MAX_FILES) {
    char name[32];
    entry.getName(name, sizeof(name));
    size_t len = strlen(name);
    // Accept .bmp and .BMP
    if (len > 4) {
      const char* ext = name + len - 4;
      if (ext[0] == '.' &&
          (ext[1] == 'b' || ext[1] == 'B') &&
          (ext[2] == 'm' || ext[2] == 'M') &&
          (ext[3] == 'p' || ext[3] == 'P')) {
        strncpy(fileNames[fileCount], name, sizeof(fileNames[0]) - 1);
        fileNames[fileCount][sizeof(fileNames[0]) - 1] = '\0';
        fileCount++;
      }
    }
    entry.close();
  }
  dir.close();
}

// ---------------------------------------------------------------------------
// Steganography: embed
// Writes BMP header bytes + new STEG payload to a temp file, then renames
// over the original. This avoids needing a truncate() call.
// ---------------------------------------------------------------------------

bool SteganographyActivity::embedMessage(const char* path, const char* msg, int len) {
  // Step 1: open original, validate BMP magic, read declared BMP size (bytes 2-5 LE)
  HalFile src = Storage.open(path, O_RDONLY);
  if (!src) return false;

  uint8_t magic[2];
  if (src.read(magic, 2) != 2 || magic[0] != 'B' || magic[1] != 'M') {
    src.close();
    embedSuccess = false;
    // Reuse messageBuffer to surface the error to the EMBED_DONE screen
    strncpy(messageBuffer, "Not a valid BMP file", MAX_MSG_LEN);
    messageBuffer[MAX_MSG_LEN] = '\0';
    messageLen = 0;
    return false;
  }

  if (!src.seekSet(2)) {
    src.close();
    return false;
  }
  uint32_t bmpSize = 0;
  if (src.read(&bmpSize, 4) != 4) {
    src.close();
    return false;
  }
  if (bmpSize < 62 || bmpSize > 1024UL * 1024UL * 4UL) {
    // Sanity check — reject obviously bogus headers
    src.close();
    return false;
  }
  src.seekSet(0);

  // Step 2: write to a temporary file next to the original
  char tmpPath[88];
  snprintf(tmpPath, sizeof(tmpPath), "%s.tmp", path);
  Storage.remove(tmpPath);  // Clean up any stale temp

  HalFile dst = Storage.open(tmpPath, O_WRITE | O_CREAT | O_TRUNC);
  if (!dst) {
    src.close();
    return false;
  }

  // Copy exactly bmpSize bytes from source to temp
  uint8_t buf[128];
  uint32_t remaining = bmpSize;
  bool copyOk = true;
  while (remaining > 0 && copyOk) {
    uint32_t toRead = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
    int got = src.read(buf, toRead);
    if (got <= 0) { copyOk = false; break; }
    size_t wrote = dst.write(buf, (size_t)got);
    if (wrote != (size_t)got) { copyOk = false; break; }
    remaining -= (uint32_t)got;
  }
  src.close();

  if (!copyOk) {
    dst.close();
    Storage.remove(tmpPath);
    return false;
  }

  // Step 3: append STEG payload
  dst.write((const uint8_t*)"STEG", 4);

  uint32_t len32 = (uint32_t)len;
  dst.write((const uint8_t*)&len32, 4);

  dst.write((const uint8_t*)msg, (size_t)len);

  uint8_t checksum = 0;
  for (int i = 0; i < len; i++) checksum ^= (uint8_t)msg[i];
  dst.write(&checksum, 1);

  dst.flush();
  dst.close();

  // Step 4: replace original with temp
  Storage.remove(path);
  if (!Storage.rename(tmpPath, path)) {
    // Rename failed — the temp file has the data, but we can't restore the
    // original name.  Log and return failure so the caller can surface it.
    LOG_ERR("Steganography", "rename %s -> %s failed", tmpPath, path);
    return false;
  }

  return true;
}

// ---------------------------------------------------------------------------
// Steganography: extract
// ---------------------------------------------------------------------------

bool SteganographyActivity::extractMessage(const char* path) {
  messageLen = 0;
  messageBuffer[0] = '\0';

  HalFile f = Storage.open(path, O_RDONLY);
  if (!f) return false;

  // Read declared BMP size
  if (!f.seekSet(2)) { f.close(); return false; }
  uint32_t bmpSize = 0;
  if (f.read(&bmpSize, 4) != 4) { f.close(); return false; }

  uint32_t fileSize = (uint32_t)f.size();
  // Need at least STEG(4) + len(4) + checksum(1) after BMP data
  if (fileSize <= bmpSize + 9U) { f.close(); return false; }

  // Seek to where the STEG marker should start
  if (!f.seekSet(bmpSize)) { f.close(); return false; }

  char magic[4];
  if (f.read(magic, 4) != 4) { f.close(); return false; }
  if (memcmp(magic, "STEG", 4) != 0) { f.close(); return false; }

  uint32_t len32 = 0;
  if (f.read(&len32, 4) != 4) { f.close(); return false; }
  if (len32 == 0 || len32 > (uint32_t)MAX_MSG_LEN) { f.close(); return false; }

  int got = f.read(messageBuffer, (size_t)len32);
  if (got != (int)len32) { f.close(); return false; }
  messageBuffer[len32] = '\0';

  uint8_t storedChecksum = 0;
  if (f.read(&storedChecksum, 1) != 1) { f.close(); return false; }
  f.close();

  uint8_t calcChecksum = 0;
  for (uint32_t i = 0; i < len32; i++) calcChecksum ^= (uint8_t)messageBuffer[i];

  if (storedChecksum != calcChecksum) {
    messageLen = 0;
    messageBuffer[0] = '\0';
    return false;
  }

  messageLen = (int)len32;
  return true;
}

// ---------------------------------------------------------------------------
// Helpers called from loop()
// ---------------------------------------------------------------------------

void SteganographyActivity::doEmbed() {
  embedSuccess = embedMessage(selectedPath, messageBuffer, messageLen);
  state = EMBED_DONE;
  requestUpdate();
}

void SteganographyActivity::doExtract() {
  extractFound = extractMessage(selectedPath);
  scrollOffset = 0;
  totalLines = 0;
  linesCacheDirty = true;
  state = EXTRACT_RESULT;
  requestUpdate();
}

// ---------------------------------------------------------------------------
// Loop
// ---------------------------------------------------------------------------

void SteganographyActivity::loop() {
  if (state == MODE_SELECT) {
    // Two items: 0 = Embed, 1 = Extract
    buttonNavigator.onNext([this] {
      mode = (mode == EMBED) ? EXTRACT : EMBED;
      requestUpdate();
    });
    buttonNavigator.onPrevious([this] {
      mode = (mode == EMBED) ? EXTRACT : EMBED;
      requestUpdate();
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      loadFileList();
      fileIndex = 0;
      state = FILE_SELECT;
      requestUpdate();
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      finish();
    }
    return;
  }

  if (state == FILE_SELECT) {
    buttonNavigator.onNext([this] {
      if (fileCount > 0) {
        fileIndex = ButtonNavigator::nextIndex(fileIndex, fileCount);
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this] {
      if (fileCount > 0) {
        fileIndex = ButtonNavigator::previousIndex(fileIndex, fileCount);
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (fileCount > 0) {
        snprintf(selectedPath, sizeof(selectedPath), "/biscuit/drawings/%s",
                 fileNames[fileIndex]);
        if (mode == EMBED) {
          // Launch keyboard to collect secret message
          startActivityForResult(
              std::make_unique<KeyboardEntryActivity>(renderer, mappedInput,
                                                     "Secret Message", "", MAX_MSG_LEN, false),
              [this](const ActivityResult& result) {
                if (!result.isCancelled) {
                  const auto& kb = std::get<KeyboardResult>(result.data);
                  int len = static_cast<int>(kb.text.size());
                  if (len > MAX_MSG_LEN) len = MAX_MSG_LEN;
                  memcpy(messageBuffer, kb.text.c_str(), (size_t)len);
                  messageBuffer[len] = '\0';
                  messageLen = len;
                  if (messageLen > 0) {
                    doEmbed();
                  } else {
                    // Empty message — go back to mode select
                    state = MODE_SELECT;
                    requestUpdate();
                  }
                } else {
                  state = MODE_SELECT;
                  requestUpdate();
                }
              });
        } else {
          doExtract();
        }
      }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MODE_SELECT;
      requestUpdate();
    }
    return;
  }

  if (state == EMBED_DONE) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MODE_SELECT;
      requestUpdate();
    }
    return;
  }

  if (state == EXTRACT_RESULT) {
    const auto& metrics = UITheme::getInstance().getMetrics();
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int pageHeight = renderer.getScreenHeight();
    const int contentTop =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    int visibleLines = contentHeight / lineH;
    if (visibleLines < 1) visibleLines = 1;

    buttonNavigator.onNext([this, visibleLines] {
      if (scrollOffset + visibleLines < totalLines) {
        scrollOffset += visibleLines;
        requestUpdate();
      }
    });
    buttonNavigator.onPrevious([this, visibleLines] {
      if (scrollOffset > 0) {
        scrollOffset -= visibleLines;
        if (scrollOffset < 0) scrollOffset = 0;
        requestUpdate();
      }
    });

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = MODE_SELECT;
      requestUpdate();
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------

void SteganographyActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight},
                 "Stego Notes");

  if (state == MODE_SELECT) {
    const int contentTop =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    static const char* const modeLabels[2] = {"Embed Message", "Extract Message"};
    GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                 2, static_cast<int>(mode),
                 [](int i) -> std::string { return modeLabels[i]; },
                 [](int i) -> std::string {
                   return (i == 0) ? "Hide text in a BMP image"
                                   : "Read hidden text from a BMP image";
                 });

    const auto labels = mappedInput.mapLabels("Back", "Select", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == FILE_SELECT) {
    const int contentTop =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (fileCount == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2,
                                "No BMP files in /biscuit/drawings");
    } else {
      GUI.drawList(renderer, Rect{0, contentTop, pageWidth, contentHeight},
                   fileCount, fileIndex,
                   [this](int i) -> std::string { return fileNames[i]; },
                   [this](int i) -> std::string {
                     char buf[48];
                     snprintf(buf, sizeof(buf), "/biscuit/drawings/%s", fileNames[i]);
                     return buf;
                   });
    }

    const char* action = (mode == EMBED) ? "Embed" : "Extract";
    const auto labels = mappedInput.mapLabels("Back", action, "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == EMBED_DONE) {
    int y = pageHeight / 2 - 40;
    if (embedSuccess) {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "Message embedded!", true,
                                EpdFontFamily::BOLD);
      y += 45;
      // Show just the filename part
      const char* slash = strrchr(selectedPath, '/');
      renderer.drawCenteredText(UI_10_FONT_ID, y, slash ? slash + 1 : selectedPath);
    } else {
      renderer.drawCenteredText(UI_12_FONT_ID, y, "Embed failed", true,
                                EpdFontFamily::BOLD);
      y += 45;
      renderer.drawCenteredText(UI_10_FONT_ID, y, "Check that the file exists and is writable");
    }

    const auto labels = mappedInput.mapLabels("Back", "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  if (state == EXTRACT_RESULT) {
    const int contentTop =
        metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight =
        pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;
    const int lineH = renderer.getLineHeight(UI_10_FONT_ID);
    const int maxWidth = pageWidth - 2 * metrics.contentSidePadding;
    int visibleLines = contentHeight / lineH;
    if (visibleLines < 1) visibleLines = 1;

    if (!extractFound || messageLen == 0) {
      renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2,
                                "No hidden message found");
    } else {
      if (linesCacheDirty) {
        cachedLines = renderer.wrappedText(UI_10_FONT_ID, messageBuffer, maxWidth, 0);
        linesCacheDirty = false;
      }
      totalLines = static_cast<int>(cachedLines.size());

      int y = contentTop;
      for (int i = scrollOffset;
           i < scrollOffset + visibleLines && i < totalLines; i++) {
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y,
                          cachedLines[i].c_str());
        y += lineH;
      }

      // Scroll indicator
      if (totalLines > visibleLines) {
        int totalPages = (totalLines + visibleLines - 1) / visibleLines;
        int currentPage = scrollOffset / visibleLines + 1;
        char pageInfo[16];
        snprintf(pageInfo, sizeof(pageInfo), "%d/%d", currentPage, totalPages);
        int pw = renderer.getTextWidth(SMALL_FONT_ID, pageInfo);
        renderer.drawText(SMALL_FONT_ID,
                          pageWidth - pw - metrics.contentSidePadding,
                          pageHeight - metrics.buttonHintsHeight - 20,
                          pageInfo);
      }
    }

    const auto labels = mappedInput.mapLabels("Back", "", "^", "v");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }
}
