#include "ReaderActivity.h"

#include <SD.h>

#include "Epub.h"
#include "EpubReaderActivity.h"
#include "FileSelectionActivity.h"
#include "../home/GridBrowserActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "../../CrossPointSettings.h"

std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

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
  currentEpubPath = path;  // Track current book path
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

void ReaderActivity::onGoToFileSelection(const std::string& fromEpubPath) {
  exitActivity();
  // If coming from a book, start in that book's folder; otherwise start from root
  const auto initialPath = fromEpubPath.empty() ? "/" : extractFolderPath(fromEpubPath);
  if (SETTINGS.uiTheme == CrossPointSettings::GRID) {
    enterNewActivity(new GridBrowserActivity(
        renderer, inputManager, [this](const std::string& path) { onSelectEpubFile(path); }, onGoBack, initialPath));
  } else {
    enterNewActivity(new FileSelectionActivity(
        renderer, inputManager, [this](const std::string& path) { onSelectEpubFile(path); }, onGoBack, initialPath));
  }
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentEpubPath = epubPath;
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, inputManager, std::move(epub), [this, epubPath] { onGoToFileSelection(epubPath); },
      [this] { onGoBack(); }));
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialEpubPath.empty()) {
    onGoToFileSelection();  // Start from root when entering via Browse
    return;
  }

  currentEpubPath = initialEpubPath;
  auto epub = loadEpub(initialEpubPath);
  if (!epub) {
    onGoBack();
    return;
  }

  onGoToEpubReader(std::move(epub));
}
