#include "Section.h"

#include <EpdRenderer.h>
#include <SD.h>

#include <fstream>

#include "EpubHtmlParserSlim.h"
#include "Page.h"

void Section::onPageComplete(const Page* page) {
  Serial.printf("Page %d complete - free mem: %lu\n", pageCount, ESP.getFreeHeap());

  const auto filePath = cachePath + "/page_" + std::to_string(pageCount) + ".bin";
  // TODO can this be removed?
  SD.open(filePath.c_str(), FILE_WRITE).close();

  std::ofstream outputFile("/sd" + filePath);
  page->serialize(outputFile);
  outputFile.close();

  pageCount++;
  delete page;
}

bool Section::hasCache() {
  if (!SD.exists(cachePath.c_str())) {
    return false;
  }

  const auto sectionFilePath = cachePath + "/section.bin";
  if (!SD.exists(sectionFilePath.c_str())) {
    return false;
  }

  File sectionFile = SD.open(sectionFilePath.c_str(), FILE_READ);
  uint8_t pageCountBytes[2] = {0, 0};
  sectionFile.read(pageCountBytes, 2);
  sectionFile.close();

  pageCount = pageCountBytes[0] + (pageCountBytes[1] << 8);
  Serial.printf("Loaded cache: %d pages\n", pageCount);

  return true;
}

void Section::setupCacheDir() const {
  epub->setupCacheDir();
  SD.mkdir(cachePath.c_str());
}

void Section::clearCache() const { SD.rmdir(cachePath.c_str()); }

bool Section::persistPageDataToSD() {
  size_t size = 0;
  auto localPath = epub->getSpineItem(spineIndex);

  const auto html = epub->getItemContents(epub->getSpineItem(spineIndex), &size);
  if (!html) {
    Serial.println("Failed to read item contents");
    return false;
  }

  // TODO: Would love to stream this through an XML visitor
  const auto tmpHtmlPath = epub->getCachePath() + "/.tmp_" + std::to_string(spineIndex) + ".html";
  File f = SD.open(tmpHtmlPath.c_str(), FILE_WRITE);
  const auto written = f.write(html, size);
  f.close();
  free(html);

  Serial.printf("Wrote %d bytes to %s\n", written, tmpHtmlPath.c_str());

  if (size != written) {
    Serial.println("Failed to inflate section contents to SD");
    SD.remove(tmpHtmlPath.c_str());
    return false;
  }

  const auto sdTmpHtmlPath = "/sd" + tmpHtmlPath;

  auto visitor =
      EpubHtmlParserSlim(sdTmpHtmlPath.c_str(), renderer, [this](const Page* page) { this->onPageComplete(page); });
  const bool success = visitor.parseAndBuildPages();

  SD.remove(tmpHtmlPath.c_str());
  if (!success) {
    Serial.println("Failed to parse and build pages");
    return false;
  }

  File sectionFile = SD.open((cachePath + "/section.bin").c_str(), FILE_WRITE, true);
  const uint8_t pageCountBytes[2] = {static_cast<uint8_t>(pageCount & 0xFF),
                                     static_cast<uint8_t>((pageCount >> 8) & 0xFF)};
  sectionFile.write(pageCountBytes, 2);
  sectionFile.close();

  return true;
}

void Section::renderPage() {
  if (0 <= currentPage && currentPage < pageCount) {
    const auto filePath = "/sd" + cachePath + "/page_" + std::to_string(currentPage) + ".bin";
    std::ifstream inputFile(filePath);
    const Page* p = Page::deserialize(inputFile);
    inputFile.close();
    p->render(renderer);
    delete p;
  } else if (pageCount == 0) {
    Serial.println("No pages to render");
    const int width = renderer.getTextWidth("Empty chapter", BOLD);
    renderer.drawText((renderer.getPageWidth() - width) / 2, 300, "Empty chapter", 1, BOLD);
  } else {
    Serial.printf("Page out of bounds: %d (max %d)\n", currentPage, pageCount);
    const int width = renderer.getTextWidth("Out of bounds", BOLD);
    renderer.drawText((renderer.getPageWidth() - width) / 2, 300, "Out of bounds", 1, BOLD);
  }
}
