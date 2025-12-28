#include "ReaderActivity.h"

#include <SD.h>

#include "Epub.h"
#include "EpubReaderActivity.h"
#include "FileSelectionActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/FullScreenMessageActivity.h"

std::string ReaderActivity::extractFolderPath(const std::string& filePath) {
  const auto lastSlash = filePath.find_last_of('/');
  if (lastSlash == std::string::npos || lastSlash == 0) {
    return "/";
  }
  return filePath.substr(0, lastSlash);
}

bool ReaderActivity::isXtcFile(const std::string& path) {
  if (path.length() < 4) return false;
  std::string ext4 = path.substr(path.length() - 4);
  if (ext4 == ".xtc") return true;
  if (path.length() >= 5) {
    std::string ext5 = path.substr(path.length() - 5);
    if (ext5 == ".xtch") return true;
  }
  return false;
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

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!SD.exists(path.c_str())) {
    Serial.printf("[%lu] [   ] File does not exist: %s\n", millis(), path.c_str());
    return nullptr;
  }

  auto xtc = std::unique_ptr<Xtc>(new Xtc(path, "/.crosspoint"));
  if (xtc->load()) {
    return xtc;
  }

  Serial.printf("[%lu] [   ] Failed to load XTC\n", millis());
  return nullptr;
}

void ReaderActivity::onSelectBookFile(const std::string& path) {
  currentBookPath = path;  // Track current book path
  exitActivity();
  enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Loading..."));

  if (isXtcFile(path)) {
    // Load XTC file
    auto xtc = loadXtc(path);
    if (xtc) {
      onGoToXtcReader(std::move(xtc));
    } else {
      exitActivity();
      enterNewActivity(new FullScreenMessageActivity(renderer, inputManager, "Failed to load XTC", REGULAR,
                                                     EInkDisplay::HALF_REFRESH));
      delay(2000);
      onGoToFileSelection();
    }
  } else {
    // Load EPUB file
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
}

void ReaderActivity::onGoToFileSelection(const std::string& fromBookPath) {
  exitActivity();
  // If coming from a book, start in that book's folder; otherwise start from root
  const auto initialPath = fromBookPath.empty() ? "/" : extractFolderPath(fromBookPath);
  enterNewActivity(new FileSelectionActivity(
      renderer, inputManager, [this](const std::string& path) { onSelectBookFile(path); }, onGoBack, initialPath));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  exitActivity();
  enterNewActivity(new EpubReaderActivity(
      renderer, inputManager, std::move(epub), [this, epubPath] { onGoToFileSelection(epubPath); },
      [this] { onGoBack(); }));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  exitActivity();
  enterNewActivity(new XtcReaderActivity(
      renderer, inputManager, std::move(xtc), [this, xtcPath] { onGoToFileSelection(xtcPath); },
      [this] { onGoBack(); }));
}

void ReaderActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  if (initialBookPath.empty()) {
    onGoToFileSelection();  // Start from root when entering via Browse
    return;
  }

  currentBookPath = initialBookPath;

  if (isXtcFile(initialBookPath)) {
    auto xtc = loadXtc(initialBookPath);
    if (!xtc) {
      onGoBack();
      return;
    }
    onGoToXtcReader(std::move(xtc));
  } else {
    auto epub = loadEpub(initialBookPath);
    if (!epub) {
      onGoBack();
      return;
    }
    onGoToEpubReader(std::move(epub));
  }
}
