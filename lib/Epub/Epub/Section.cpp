#include "Section.h"

#include <HalStorage.h>
#include <Hyphenator.h>
#include <Logging.h>
#include <Typesetter/Page.h>

#include "Epub/css/CssParser.h"
#include "parsers/ChapterHtmlSlimParser.h"

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled) {
  return cache.loadHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                          viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
}

bool Section::clearCache() const { return cache.clearCache(); }

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const std::function<void()>& popupFn) {
  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Retry logic for SD card timing issues
  bool success = false;
  uint32_t fileSize = 0;
  for (int attempt = 0; attempt < 3 && !success; attempt++) {
    if (attempt > 0) {
      LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
      delay(50);
    }

    if (Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }

    FsFile tmpHtml;
    if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
      continue;
    }
    success = epub->readItemContentsToStream(localPath, tmpHtml, 1024);
    fileSize = tmpHtml.size();
    tmpHtml.close();

    if (!success && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
      LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
    }
  }

  if (!success) {
    LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
    return false;
  }

  LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

  if (!cache.openForWrite()) {
    return false;
  }
  cache.writeHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                    hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled);
  std::vector<PageLutEntry> lut = {};

  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = epub->getCssParser();
    if (cssParser) {
      if (!cssParser->loadFromCache()) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  ChapterHtmlSlimParser visitor(
      epub, tmpHtmlPath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
      viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, &lut](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        lut.push_back({cache.writePage(std::move(page)), paragraphIndex, listItemIndex});
      },
      embeddedStyle, contentBase, imageBasePath, imageRendering, popupFn, cssParser);
  Hyphenator::setPreferredLanguage(epub->getLanguage());
  success = visitor.parseAndBuildPages();

  Storage.remove(tmpHtmlPath.c_str());
  if (!success) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    cache.closeAndRemove();
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  if (!cache.finalize(lut, visitor.getAnchors())) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  if (cssParser) {
    cssParser->clear();
  }
  return true;
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() { return cache.loadPage(currentPage); }

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  return cache.getPageForAnchor(anchor);
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  return cache.getPageForParagraphIndex(pIndex);
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  return cache.getPageForListItemIndex(liIndex);
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  return cache.getParagraphIndexForPage(page);
}
