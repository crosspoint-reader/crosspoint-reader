#include "ReaderActivity.h"

#include <SD.h>

#include "Epub.h"
#include "EpubReaderActivity.h"
#include "FileSelectionActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!SD.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto epub = std::unique_ptr<Epub>(new Epub(path, "/.crosspoint"));
  if (epub->load()) {
    return epub;
  }

  Serial.printf("[%lu] [   ] Failed to load epub\n", millis());
  return nullptr;
}

void ReaderActivity::onSelectEpubFile(const std::string& path) {
  exitActivity();
  enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Loading..."));

  auto epub = loadEpub(path);
  if (epub) {
    onGoToEpubReader(std::move(epub));
  } else {
    exitActivity();
    enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Failed to load epub", REGULAR,
                                                   EInkDisplay::HALF_REFRESH));
    delay(2000);
    onGoToFileSelection();
  }
}

void ReaderActivity::onGoToFileSelection() {
  exitActivity();
  enterNewActivity(new FileSelectionActivity(
      renderer, inputManager, [this](const std::string& path) { onSelectEpubFile(path); }, onGoBack));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, inputManager, std::move(epub), [this] { onGoToFileSelection(); }, [this] { onGoBack(); }));
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialEpubPath.empty()) {
    onGoToFileSelection();
    return;
  }

  auto epub = loadEpub(initialEpubPath);
  if (!epub) {
    onGoBack();
    return;
  }

  onGoToEpubReader(std::move(epub));
}
