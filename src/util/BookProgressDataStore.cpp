#include "util/BookProgressDataStore.h"

#include <HalStorage.h>
#include <Serialization.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>

namespace {
constexpr char kCacheBasePath[] = "/.crosspoint";
constexpr char kProgressFileName[] = "/progress.bin";
constexpr char kTxtIndexFileName[] = "/index.bin";
constexpr char kMarkdownSectionFileName[] = "/md_section.bin";
constexpr char kHtmlSectionFileName[] = "/section.bin";
constexpr char kEpubBookCacheFileName[] = "/book.bin";

constexpr uint32_t kTxtCacheMagic = 0x54585449;  // "TXTI"
constexpr uint8_t kTxtCacheVersion = 3;
constexpr uint8_t kSectionFileVersion = 1;
constexpr uint8_t kEpubBookCacheVersion = 5;
constexpr uint32_t kMaxSerializedStringLength = 64 * 1024;
constexpr uint32_t kDiscardBufferSize = 64;

constexpr uint32_t kXtcMagic = 0x00435458;   // "XTC\0"
constexpr uint32_t kXtchMagic = 0x48435458;  // "XTCH"

#pragma pack(push, 1)
struct XtcHeaderPrefix {
  uint32_t magic;
  uint8_t versionMajor;
  uint8_t versionMinor;
  uint16_t pageCount;
};
#pragma pack(pop)

bool hasExtension(const std::string& bookPath, const char* extension) {
  const size_t extLen = std::strlen(extension);
  if (bookPath.size() < extLen) {
    return false;
  }

  const size_t start = bookPath.size() - extLen;
  for (size_t i = 0; i < extLen; ++i) {
    const unsigned char lhs = static_cast<unsigned char>(bookPath[start + i]);
    const unsigned char rhs = static_cast<unsigned char>(extension[i]);
    if (std::tolower(lhs) != std::tolower(rhs)) {
      return false;
    }
  }

  return true;
}

BookProgressDataStore::BookKind detectBookKind(const std::string& bookPath) {
  if (hasExtension(bookPath, ".epub")) {
    return BookProgressDataStore::BookKind::Epub;
  }
  if (hasExtension(bookPath, ".txt")) {
    return BookProgressDataStore::BookKind::Txt;
  }
  if (hasExtension(bookPath, ".md")) {
    return BookProgressDataStore::BookKind::Markdown;
  }
  if (hasExtension(bookPath, ".xtc") || hasExtension(bookPath, ".xtch")) {
    return BookProgressDataStore::BookKind::Xtc;
  }
  return BookProgressDataStore::BookKind::Unknown;
}

const char* cachePrefixForKind(const BookProgressDataStore::BookKind kind) {
  switch (kind) {
    case BookProgressDataStore::BookKind::Epub:
      return "epub_";
    case BookProgressDataStore::BookKind::Txt:
      return "txt_";
    case BookProgressDataStore::BookKind::Markdown:
      return "md_";
    case BookProgressDataStore::BookKind::Xtc:
      return "xtc_";
    case BookProgressDataStore::BookKind::Unknown:
    default:
      return nullptr;
  }
}

std::string buildCachePath(const BookProgressDataStore::BookKind kind, const std::string& bookPath) {
  const char* prefix = cachePrefixForKind(kind);
  if (prefix == nullptr) {
    return "";
  }
  return std::string(kCacheBasePath) + "/" + prefix + std::to_string(std::hash<std::string>{}(bookPath));
}

float clampPercent(const float percent) {
  const float clamped = std::clamp(percent, 0.0f, 100.0f);
  return std::round(clamped * 100.0f) / 100.0f;
}

uint32_t clampDisplayPage(const uint32_t zeroBasedPage, const uint32_t pageCount) {
  if (pageCount == 0) {
    return 0;
  }
  return std::min(zeroBasedPage + 1, pageCount);
}

bool skipBytes(FsFile& file, size_t bytesToSkip) {
  uint8_t discard[kDiscardBufferSize];
  while (bytesToSkip > 0) {
    const size_t chunk = std::min(bytesToSkip, static_cast<size_t>(kDiscardBufferSize));
    if (file.read(discard, chunk) != chunk) {
      return false;
    }
    bytesToSkip -= chunk;
  }
  return true;
}

bool skipSerializedString(FsFile& file) {
  uint32_t len = 0;
  if (!serialization::readPod(file, len)) {
    return false;
  }
  if (len > kMaxSerializedStringLength) {
    return false;
  }
  return skipBytes(file, len);
}

bool loadTxtProgressFromCache(const std::string& cachePath, const BookProgressDataStore::BookKind logicalKind,
                              BookProgressDataStore::ProgressData& outProgress) {
  FsFile progressFile;
  if (!Storage.openFileForRead("BPS", cachePath + kProgressFileName, progressFile)) {
    return false;
  }

  uint8_t progressBytes[4];
  if (progressFile.read(progressBytes, sizeof(progressBytes)) != sizeof(progressBytes)) {
    progressFile.close();
    return false;
  }
  progressFile.close();

  FsFile indexFile;
  if (!Storage.openFileForRead("BPS", cachePath + kTxtIndexFileName, indexFile)) {
    return false;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t ignoredU32 = 0;
  int32_t ignoredI32 = 0;
  uint8_t ignoredU8 = 0;
  uint32_t totalPages = 0;

  const bool ok = serialization::readPod(indexFile, magic) && serialization::readPod(indexFile, version) &&
                  serialization::readPod(indexFile, ignoredU32) && serialization::readPod(indexFile, ignoredI32) &&
                  serialization::readPod(indexFile, ignoredI32) && serialization::readPod(indexFile, ignoredI32) &&
                  serialization::readPod(indexFile, ignoredI32) && serialization::readPod(indexFile, ignoredU8) &&
                  serialization::readPod(indexFile, ignoredU8) && serialization::readPod(indexFile, totalPages);
  indexFile.close();

  if (!ok || magic != kTxtCacheMagic || version != kTxtCacheVersion || totalPages == 0) {
    return false;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressBytes[0]) | (static_cast<uint32_t>(progressBytes[1]) << 8);
  const uint32_t page = clampDisplayPage(currentPage, totalPages);

  outProgress.kind = logicalKind;
  outProgress.page = page;
  outProgress.pageCount = totalPages;
  outProgress.percent = clampPercent(static_cast<float>(page) * 100.0f / static_cast<float>(totalPages));
  outProgress.spineIndex = -1;
  return true;
}

bool loadSectionProgressFromFile(const std::string& progressPath, const std::string& sectionPath,
                                 BookProgressDataStore::ProgressData& outProgress) {
  FsFile progressFile;
  if (!Storage.openFileForRead("BPS", progressPath, progressFile)) {
    return false;
  }

  uint8_t progressBytes[4];
  if (progressFile.read(progressBytes, sizeof(progressBytes)) != sizeof(progressBytes)) {
    progressFile.close();
    return false;
  }
  progressFile.close();

  FsFile sectionFile;
  if (!Storage.openFileForRead("BPS", sectionPath, sectionFile)) {
    return false;
  }

  uint8_t version = 0;
  int ignoredFontId = 0;
  float ignoredLineCompression = 0.0f;
  bool ignoredExtraParagraphSpacing = false;
  uint8_t ignoredParagraphAlignment = 0;
  uint16_t ignoredViewportWidth = 0;
  uint16_t ignoredViewportHeight = 0;
  bool ignoredHyphenation = false;
  uint32_t ignoredSourceSize = 0;
  uint16_t pageCount = 0;

  const bool ok = serialization::readPod(sectionFile, version) && serialization::readPod(sectionFile, ignoredFontId) &&
                  serialization::readPod(sectionFile, ignoredLineCompression) &&
                  serialization::readPod(sectionFile, ignoredExtraParagraphSpacing) &&
                  serialization::readPod(sectionFile, ignoredParagraphAlignment) &&
                  serialization::readPod(sectionFile, ignoredViewportWidth) &&
                  serialization::readPod(sectionFile, ignoredViewportHeight) &&
                  serialization::readPod(sectionFile, ignoredHyphenation) &&
                  serialization::readPod(sectionFile, ignoredSourceSize) &&
                  serialization::readPod(sectionFile, pageCount);
  sectionFile.close();

  if (!ok || version != kSectionFileVersion || pageCount == 0) {
    return false;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressBytes[0]) | (static_cast<uint32_t>(progressBytes[1]) << 8);
  const uint32_t displayPage = clampDisplayPage(currentPage, pageCount);

  outProgress.kind = BookProgressDataStore::BookKind::Markdown;
  outProgress.page = displayPage;
  outProgress.pageCount = pageCount;
  outProgress.percent = clampPercent(static_cast<float>(displayPage) * 100.0f / static_cast<float>(pageCount));
  outProgress.spineIndex = -1;
  return true;
}

bool loadMarkdownProgressFromCache(const std::string& cachePath, BookProgressDataStore::ProgressData& outProgress) {
  const std::string progressPath = cachePath + kProgressFileName;
  if (loadSectionProgressFromFile(progressPath, cachePath + kMarkdownSectionFileName, outProgress)) {
    return true;
  }
  return loadSectionProgressFromFile(progressPath, cachePath + kHtmlSectionFileName, outProgress);
}

bool loadXtcPageCount(const std::string& bookPath, uint32_t& outPageCount) {
  FsFile bookFile;
  if (!Storage.openFileForRead("BPS", bookPath, bookFile)) {
    return false;
  }

  XtcHeaderPrefix header{};
  const bool ok = bookFile.read(reinterpret_cast<uint8_t*>(&header), sizeof(header)) == sizeof(header);
  bookFile.close();

  if (!ok) {
    return false;
  }

  const bool validMagic = header.magic == kXtcMagic || header.magic == kXtchMagic;
  const bool validVersion =
      (header.versionMajor == 1 && header.versionMinor == 0) || (header.versionMajor == 0 && header.versionMinor == 1);
  if (!validMagic || !validVersion || header.pageCount == 0) {
    return false;
  }

  outPageCount = header.pageCount;
  return true;
}

bool loadXtcProgressFromCache(const std::string& bookPath, const std::string& cachePath,
                              BookProgressDataStore::ProgressData& outProgress) {
  FsFile progressFile;
  if (!Storage.openFileForRead("BPS", cachePath + kProgressFileName, progressFile)) {
    return false;
  }

  uint8_t progressBytes[4];
  if (progressFile.read(progressBytes, sizeof(progressBytes)) != sizeof(progressBytes)) {
    progressFile.close();
    return false;
  }
  progressFile.close();

  uint32_t pageCount = 0;
  if (!loadXtcPageCount(bookPath, pageCount) || pageCount == 0) {
    return false;
  }

  const uint32_t currentPage =
      static_cast<uint32_t>(progressBytes[0]) | (static_cast<uint32_t>(progressBytes[1]) << 8) |
      (static_cast<uint32_t>(progressBytes[2]) << 16) | (static_cast<uint32_t>(progressBytes[3]) << 24);
  const uint32_t displayPage = clampDisplayPage(currentPage, pageCount);

  outProgress.kind = BookProgressDataStore::BookKind::Xtc;
  outProgress.page = displayPage;
  outProgress.pageCount = pageCount;
  outProgress.percent = clampPercent(static_cast<float>(displayPage) * 100.0f / static_cast<float>(pageCount));
  outProgress.spineIndex = -1;
  return true;
}

bool loadEpubProgressFromCache(const std::string& cachePath, BookProgressDataStore::ProgressData& outProgress) {
  FsFile progressFile;
  if (!Storage.openFileForRead("BPS", cachePath + kProgressFileName, progressFile)) {
    return false;
  }

  uint8_t progressBytes[6];
  if (progressFile.read(progressBytes, sizeof(progressBytes)) != sizeof(progressBytes)) {
    progressFile.close();
    return false;
  }
  progressFile.close();

  const uint32_t spineIndex = static_cast<uint32_t>(progressBytes[0]) | (static_cast<uint32_t>(progressBytes[1]) << 8);
  const uint32_t currentPage = static_cast<uint32_t>(progressBytes[2]) | (static_cast<uint32_t>(progressBytes[3]) << 8);
  const uint32_t sectionPageCount =
      static_cast<uint32_t>(progressBytes[4]) | (static_cast<uint32_t>(progressBytes[5]) << 8);
  if (sectionPageCount == 0) {
    return false;
  }

  FsFile bookFile;
  if (!Storage.openFileForRead("BPS", cachePath + kEpubBookCacheFileName, bookFile)) {
    return false;
  }

  uint8_t version = 0;
  uint32_t ignoredLutOffset = 0;
  uint16_t spineCount = 0;
  uint16_t tocCount = 0;
  const bool headerOk = serialization::readPod(bookFile, version) &&
                        serialization::readPod(bookFile, ignoredLutOffset) &&
                        serialization::readPod(bookFile, spineCount) && serialization::readPod(bookFile, tocCount);
  if (!headerOk || version != kEpubBookCacheVersion || spineCount == 0 || spineIndex >= spineCount) {
    bookFile.close();
    return false;
  }

  for (int i = 0; i < 5; ++i) {
    if (!skipSerializedString(bookFile)) {
      bookFile.close();
      return false;
    }
  }

  if (!skipBytes(bookFile, sizeof(uint32_t) * (static_cast<size_t>(spineCount) + static_cast<size_t>(tocCount)))) {
    bookFile.close();
    return false;
  }

  uint32_t previousCumulativeSize = 0;
  uint32_t currentCumulativeSize = 0;
  uint32_t bookSize = 0;

  for (uint32_t i = 0; i < spineCount; ++i) {
    if (!skipSerializedString(bookFile)) {
      bookFile.close();
      return false;
    }

    uint32_t cumulativeSize = 0;
    int16_t ignoredTocIndex = -1;
    if (!serialization::readPod(bookFile, cumulativeSize) || !serialization::readPod(bookFile, ignoredTocIndex)) {
      bookFile.close();
      return false;
    }

    if (i + 1 == spineIndex) {
      previousCumulativeSize = cumulativeSize;
    }
    if (i == spineIndex) {
      currentCumulativeSize = cumulativeSize;
    }
    bookSize = cumulativeSize;
  }
  bookFile.close();

  if (bookSize == 0 || currentCumulativeSize <= previousCumulativeSize) {
    return false;
  }

  const uint32_t displayPage = clampDisplayPage(currentPage, sectionPageCount);
  const float sectionProgress = static_cast<float>(displayPage) / static_cast<float>(sectionPageCount);
  const uint32_t currentSectionSize = currentCumulativeSize - previousCumulativeSize;
  const float totalProgress =
      static_cast<float>(previousCumulativeSize) + (sectionProgress * static_cast<float>(currentSectionSize));

  outProgress.kind = BookProgressDataStore::BookKind::Epub;
  outProgress.page = displayPage;
  outProgress.pageCount = sectionPageCount;
  outProgress.percent = clampPercent(totalProgress * 100.0f / static_cast<float>(bookSize));
  outProgress.spineIndex = static_cast<int32_t>(spineIndex);
  return true;
}
}  // namespace

bool BookProgressDataStore::supportsBookPath(const std::string& bookPath) {
  return detectBookKind(bookPath) != BookKind::Unknown;
}

bool BookProgressDataStore::resolveCachePath(const std::string& bookPath, std::string& outCachePath) {
  const BookKind kind = detectBookKind(bookPath);
  outCachePath = buildCachePath(kind, bookPath);
  return !outCachePath.empty();
}

bool BookProgressDataStore::loadProgress(const std::string& bookPath, ProgressData& outProgress) {
  outProgress = {};

  const BookKind kind = detectBookKind(bookPath);
  if (kind == BookKind::Unknown) {
    return false;
  }

  switch (kind) {
    case BookKind::Epub:
      return loadEpubProgressFromCache(buildCachePath(BookKind::Epub, bookPath), outProgress);

    case BookKind::Txt:
      return loadTxtProgressFromCache(buildCachePath(BookKind::Txt, bookPath), BookKind::Txt, outProgress);

    case BookKind::Markdown:
      if (loadMarkdownProgressFromCache(buildCachePath(BookKind::Markdown, bookPath), outProgress)) {
        return true;
      }
      return loadTxtProgressFromCache(buildCachePath(BookKind::Txt, bookPath), BookKind::Markdown, outProgress);

    case BookKind::Xtc:
      return loadXtcProgressFromCache(bookPath, buildCachePath(BookKind::Xtc, bookPath), outProgress);

    case BookKind::Unknown:
    default:
      return false;
  }
}

const char* BookProgressDataStore::kindName(const BookKind kind) {
  switch (kind) {
    case BookKind::Epub:
      return "epub";
    case BookKind::Txt:
      return "txt";
    case BookKind::Markdown:
      return "markdown";
    case BookKind::Xtc:
      return "xtc";
    case BookKind::Unknown:
    default:
      return "unknown";
  }
}

std::string BookProgressDataStore::formatPositionLabel(const ProgressData& progress) {
  if (progress.pageCount == 0) {
    return "";
  }

  char buffer[64];
  const long roundedPercent = static_cast<long>(std::lround(progress.percent));
  if (progress.kind == BookKind::Epub && progress.spineIndex >= 0) {
    std::snprintf(buffer, sizeof(buffer), "Ch %ld %lu/%lu %ld%%", static_cast<long>(progress.spineIndex) + 1,
                  static_cast<unsigned long>(progress.page), static_cast<unsigned long>(progress.pageCount),
                  roundedPercent);
    return buffer;
  }

  std::snprintf(buffer, sizeof(buffer), "%lu/%lu %ld%%", static_cast<unsigned long>(progress.page),
                static_cast<unsigned long>(progress.pageCount), roundedPercent);
  return buffer;
}
