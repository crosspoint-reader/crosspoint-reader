#include "PdfCache.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Serialization.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint8_t kMetaVersion = 3;
constexpr size_t kFileHashChunkSize = 64;
constexpr uint8_t kPageVersionV1 = 1;
constexpr uint8_t kPageVersionV2 = 2;
constexpr uint8_t kPageVersionV3 = 3;
constexpr uint8_t kPageVersionV4 = 4;
constexpr uint8_t kPageVersionV5 = 5;

size_t hashPathCstr(const char* s) {
  size_t h = 5381;
  if (!s) return h;
  while (*s) {
    h = ((h << 5) + h) + static_cast<unsigned char>(*s++);
  }
  return h;
}

template <size_t N>
bool appendUnsigned(PdfFixedString<N>& s, size_t value) {
  char buf[32];
  size_t len = 0;
  do {
    if (len >= sizeof(buf)) {
      return false;
    }
    buf[len++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  } while (value != 0);
  while (len > 0) {
    if (!s.append(&buf[len - 1], 1)) {
      return false;
    }
    --len;
  }
  return true;
}

template <size_t N>
bool readFixedString(FsFile& f, PdfFixedString<N>& s) {
  uint32_t len = 0;
  serialization::readPod(f, len);
  if (len >= N) {
    return false;
  }
  if (!s.resize(len)) {
    return false;
  }
  if (len > 0) {
    if (f.read(reinterpret_cast<uint8_t*>(s.data()), len) != static_cast<int>(len)) {
      return false;
    }
  }
  s.data()[len] = '\0';
  return true;
}

uint32_t hashBytes(const uint8_t* data, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= static_cast<uint32_t>(data[i]);
    h *= 16777619u;
  }
  return h;
}

template <size_t N>
bool writeFixedString(FsFile& f, const PdfFixedString<N>& s) {
  const uint32_t len = static_cast<uint32_t>(s.size());
  serialization::writePod(f, len);
  if (len > 0) {
    f.write(reinterpret_cast<const uint8_t*>(s.data()), len);
  }
  return true;
}

template <size_t N, typename Writer>
bool saveAtomic(const char* moduleName, const PdfFixedString<N>& path, const char* tempSuffix, Writer&& writeFn) {
  PdfFixedString<N> tmpPath = path;
  if (!tmpPath.append(tempSuffix, std::strlen(tempSuffix))) {
    return false;
  }
  Storage.remove(tmpPath.c_str());
  FsFile f;
  if (!Storage.openFileForWrite(moduleName, tmpPath.c_str(), f)) {
    return false;
  }
  const bool ok = writeFn(f);
  f.flush();
  f.close();
  if (!ok) {
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (Storage.rename(tmpPath.c_str(), path.c_str())) {
    return true;
  }
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
    if (Storage.rename(tmpPath.c_str(), path.c_str())) {
      return true;
    }
  }
  Storage.remove(tmpPath.c_str());
  return false;
}

}  // namespace

void PdfCache::configure(const char* pdfFilePath, size_t fileSize) {
  cacheDir.clear();
  if (!pdfFilePath) {
    return;
  }
  const size_t hash = hashPathCstr(pdfFilePath);
  constexpr size_t kCachePrefixLen = sizeof("/.crosspoint/pdf_") - 1;
  if (!cacheDir.assign("/.crosspoint/pdf_", kCachePrefixLen) || !appendUnsigned(cacheDir, hash)) {
    cacheDir.clear();
    return;
  }
  if (fileSize != 0 && !cacheDir.empty()) {
    if (!cacheDir.append("_", 1) || !appendUnsigned(cacheDir, fileSize)) {
      cacheDir.clear();
    }
  }
}

bool PdfCache::loadMeta(uint32_t& pageCount, PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outline,
                        PdfFixedVector<uint32_t, PDF_MAX_PAGES>* pageObjectIds,
                        uint32_t* fileSize,
                        uint32_t* signatureHead,
                        uint32_t* signatureTail) {
  outline.clear();
  if (pageObjectIds) {
    pageObjectIds->clear();
  }
  if (fileSize) {
    *fileSize = 0;
  }
  if (signatureHead) {
    *signatureHead = 0;
  }
  if (signatureTail) {
    *signatureTail = 0;
  }
  PdfFixedString<PDF_MAX_PATH> path = cacheDir;
  if (!path.append("/meta.bin", 9)) {
    return false;
  }
  FsFile f;
  if (!Storage.openFileForRead("PDF", path.c_str(), f)) {
    return false;
  }
  uint8_t ver = 0;
  serialization::readPod(f, ver);
  if (ver != kMetaVersion) {
    f.close();
    return false;
  }
  serialization::readPod(f, pageCount);
  uint32_t outlineCount = 0;
  serialization::readPod(f, outlineCount);
  if (outlineCount > PDF_MAX_OUTLINE_ENTRIES) {
    f.close();
    return false;
  }
  for (uint32_t i = 0; i < outlineCount; ++i) {
    PdfOutlineEntry e;
    serialization::readPod(f, e.pageNum);
    if (!readFixedString(f, e.title)) {
      f.close();
      return false;
    }
    if (!outline.push_back(std::move(e))) {
      f.close();
      return false;
    }
  }

  if (ver >= 3) {
    if (!pageObjectIds) {
      uint32_t unused = 0;
      serialization::readPod(f, unused);  // fileSize
      serialization::readPod(f, unused);  // signatureHead
      serialization::readPod(f, unused);  // signatureTail
      uint32_t pageObjectCount = 0;
      serialization::readPod(f, pageObjectCount);
      for (uint32_t i = 0; i < pageObjectCount; ++i) {
        uint32_t ignoredId = 0;
        serialization::readPod(f, ignoredId);
      }
      f.close();
      return true;
    }

    serialization::readPod(f, *fileSize);
    serialization::readPod(f, *signatureHead);
    serialization::readPod(f, *signatureTail);
    uint32_t pageObjectCount = 0;
    serialization::readPod(f, pageObjectCount);
    if (pageObjectCount > PDF_MAX_PAGES) {
      f.close();
      return false;
    }
    for (uint32_t i = 0; i < pageObjectCount; ++i) {
      uint32_t id = 0;
      serialization::readPod(f, id);
      if (!pageObjectIds->push_back(id)) {
        f.close();
        return false;
      }
    }
  }
  f.close();
  return true;
}

bool PdfCache::saveMeta(uint32_t pageCount,
                        const PdfFixedVector<PdfOutlineEntry, PDF_MAX_OUTLINE_ENTRIES>& outline,
                        const PdfFixedVector<uint32_t, PDF_MAX_PAGES>* pageObjectIds,
                        uint32_t fileSize,
                        uint32_t signatureHead,
                        uint32_t signatureTail) {
  Storage.ensureDirectoryExists(cacheDir.c_str());
  PdfFixedString<PDF_MAX_PATH> path = cacheDir;
  if (!path.append("/meta.bin", 9)) {
    return false;
  }
  return saveAtomic("PDF", path, ".tmp", [&](FsFile& f) {
    serialization::writePod(f, kMetaVersion);
    serialization::writePod(f, pageCount);
    const uint32_t outlineCount =
        static_cast<uint32_t>(std::min(outline.size(), static_cast<size_t>(PDF_MAX_OUTLINE_ENTRIES)));
    serialization::writePod(f, outlineCount);
    for (uint32_t i = 0; i < outlineCount; ++i) {
      serialization::writePod(f, outline[i].pageNum);
      if (!writeFixedString(f, outline[i].title)) {
        return false;
      }
    }
    if (pageObjectIds) {
      serialization::writePod(f, fileSize);
      serialization::writePod(f, signatureHead);
      serialization::writePod(f, signatureTail);
      const uint32_t pageObjectCount =
          static_cast<uint32_t>(std::min(pageObjectIds->size(), static_cast<size_t>(PDF_MAX_PAGES)));
      serialization::writePod(f, pageObjectCount);
      for (uint32_t i = 0; i < pageObjectCount; ++i) {
        serialization::writePod(f, (*pageObjectIds)[i]);
      }
    } else {
      serialization::writePod(f, static_cast<uint32_t>(0));
      serialization::writePod(f, static_cast<uint32_t>(0));
      serialization::writePod(f, static_cast<uint32_t>(0));
      serialization::writePod(f, static_cast<uint32_t>(0));
    }
    return true;
  });
}

bool PdfCache::loadPage(uint32_t pageNum, PdfPage& outPage) {
  outPage.clear();

  PdfFixedString<PDF_MAX_PATH> path = cacheDir;
  if (!path.append("/pages/", 7) || !appendUnsigned(path, static_cast<size_t>(pageNum)) || !path.append(".bin", 4)) {
    return false;
  }
  if (!Storage.exists(path.c_str())) {
    return false;
  }
  FsFile f;
  if (!Storage.openFileForRead("PDF", path.c_str(), f)) {
    return false;
  }

  uint8_t ver = 0;
  serialization::readPod(f, ver);
  if (ver != kPageVersionV5) {
    f.close();
    return false;
  }

  uint32_t textCount = 0;
  serialization::readPod(f, textCount);
  if (textCount > PDF_MAX_TEXT_BLOCKS) {
    f.close();
    return false;
  }
  if (!outPage.textBlocks.resize(textCount)) {
    f.close();
    return false;
  }
  for (uint32_t i = 0; i < textCount; ++i) {
    if (!readFixedString(f, outPage.textBlocks[i].text)) {
      f.close();
      return false;
    }
    if (ver >= kPageVersionV5) {
      serialization::readPod(f, outPage.textBlocks[i].style);
    } else {
      outPage.textBlocks[i].style = PdfTextStyleRegular;
    }
    serialization::readPod(f, outPage.textBlocks[i].orderHint);
  }

  uint32_t imageCount = 0;
  serialization::readPod(f, imageCount);
  if (imageCount > PDF_MAX_IMAGES_PER_PAGE) {
    f.close();
    return false;
  }
  if (!outPage.images.resize(imageCount)) {
    f.close();
    return false;
  }
  for (uint32_t i = 0; i < imageCount; ++i) {
    serialization::readPod(f, outPage.images[i].pdfStreamOffset);
    serialization::readPod(f, outPage.images[i].pdfStreamLength);
    serialization::readPod(f, outPage.images[i].width);
    serialization::readPod(f, outPage.images[i].height);
    serialization::readPod(f, outPage.images[i].format);
  }

  if (ver >= kPageVersionV2) {
    uint32_t drawCount = 0;
    serialization::readPod(f, drawCount);
    if (drawCount > PDF_MAX_DRAW_STEPS) {
      f.close();
      return false;
    }
    if (!outPage.drawOrder.resize(drawCount)) {
      f.close();
      return false;
    }
    for (uint32_t i = 0; i < drawCount; ++i) {
      uint8_t im = 0;
      serialization::readPod(f, im);
      serialization::readPod(f, outPage.drawOrder[i].index);
      outPage.drawOrder[i].isImage = im != 0;
    }
  } else {
    for (uint32_t i = 0; i < textCount; ++i) {
      if (!outPage.drawOrder.push_back({false, i})) {
        f.close();
        return false;
      }
    }
    for (uint32_t j = 0; j < imageCount; ++j) {
      if (!outPage.drawOrder.push_back({true, j})) {
        f.close();
        return false;
      }
    }
  }

  f.close();
  return true;
}

bool PdfCache::savePage(uint32_t pageNum, const PdfPage& page) {
  PdfFixedString<PDF_MAX_PATH> pagesDir = cacheDir;
  if (!pagesDir.append("/pages", 6)) {
    return false;
  }
  Storage.ensureDirectoryExists(cacheDir.c_str());
  Storage.ensureDirectoryExists(pagesDir.c_str());

  PdfFixedString<PDF_MAX_PATH> path = pagesDir;
  if (!path.append("/", 1) || !appendUnsigned(path, static_cast<size_t>(pageNum)) || !path.append(".bin", 4)) {
    return false;
  }
  return saveAtomic("PDF", path, ".tmp", [&](FsFile& f) {
    serialization::writePod(f, kPageVersionV5);
    const uint32_t textCount = static_cast<uint32_t>(page.textBlocks.size());
    serialization::writePod(f, textCount);
    for (uint32_t i = 0; i < textCount; ++i) {
      if (!writeFixedString(f, page.textBlocks[i].text)) {
        return false;
      }
      serialization::writePod(f, page.textBlocks[i].style);
      serialization::writePod(f, page.textBlocks[i].orderHint);
    }
    const uint32_t imageCount = static_cast<uint32_t>(page.images.size());
    serialization::writePod(f, imageCount);
    for (uint32_t i = 0; i < imageCount; ++i) {
      serialization::writePod(f, page.images[i].pdfStreamOffset);
      serialization::writePod(f, page.images[i].pdfStreamLength);
      serialization::writePod(f, page.images[i].width);
      serialization::writePod(f, page.images[i].height);
      serialization::writePod(f, page.images[i].format);
    }
    const uint32_t drawCount = static_cast<uint32_t>(page.drawOrder.size());
    serialization::writePod(f, drawCount);
    for (uint32_t i = 0; i < drawCount; ++i) {
      const uint8_t im = page.drawOrder[i].isImage ? uint8_t{1} : uint8_t{0};
      serialization::writePod(f, im);
      serialization::writePod(f, page.drawOrder[i].index);
    }
    return true;
  });
}

bool PdfCache::loadProgress(uint32_t& currentPage) {
  PdfFixedString<PDF_MAX_PATH> path = cacheDir;
  if (!path.append("/progress.bin", 13)) {
    currentPage = 0;
    return false;
  }
  FsFile f;
  if (!Storage.openFileForRead("PDF", path.c_str(), f)) {
    currentPage = 0;
    return false;
  }
  serialization::readPod(f, currentPage);
  f.close();
  return true;
}

bool PdfCache::saveProgress(uint32_t currentPage) {
  Storage.ensureDirectoryExists(cacheDir.c_str());
  PdfFixedString<PDF_MAX_PATH> path = cacheDir;
  if (!path.append("/progress.bin", 13)) {
    return false;
  }
  return saveAtomic("PDF", path, ".tmp", [&](FsFile& f) {
    serialization::writePod(f, currentPage);
    return true;
  });
}

void PdfCache::invalidate() { Storage.removeDir(cacheDir.c_str()); }

bool PdfCache::allPagesCached(uint32_t pageCount) const {
  if (pageCount > PDF_MAX_PAGES) {
    return false;
  }

  for (uint32_t page = 0; page < pageCount; ++page) {
    PdfFixedString<PDF_MAX_PATH> path = cacheDir;
    if (!path.append("/pages/", 7) || !appendUnsigned(path, static_cast<size_t>(page)) || !path.append(".bin", 4)) {
      return false;
    }
    if (!Storage.exists(path.c_str())) {
      return false;
    }
  }
  return true;
}
