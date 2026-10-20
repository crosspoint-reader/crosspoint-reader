#include "Epub.h"

#include <Bitmap.h>
#include <BufferedFileIO.h>
#include <CooperativeAbort.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <Serialization.h>
#include <SidecarFiles.h>
#include <ZipFile.h>
#include <esp_system.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <deque>
#include <limits>
#include <optional>

#include "Epub/CoverImageRefScanner.h"
#include "Epub/CoverThumbSession.h"
#include "Epub/HashUtils.h"
#include "Epub/ImageFormatDetector.h"
#include "Epub/MetadataSidecar.h"
#include "Epub/SpineHrefMatch.h"
#include "Epub/SpinePageIndex.h"
#include "Epub/TocFallbackPolicy.h"
#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/PageListSink.h"
#include "Epub/parsers/PageMapParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {

// Bytes ImageFormatDetector::detect(FsFile&) sniffs. Also the threshold at which "this is not a
// format we support" becomes a PERMANENT answer rather than "the extractor hasn't got that far yet".
constexpr uint32_t COVER_FORMAT_SNIFF_BYTES = ImageFormatDetector::kSniffBytes;

bool coverImageComplete(FsFile& imageFile, ImageFormatDetector::Format format) {
  const uint32_t size = imageFile.size();
  if (format == ImageFormatDetector::Format::Jpeg) {
    constexpr uint32_t TAIL_BYTES = 64;
    const uint32_t tailSize = std::min(size, TAIL_BYTES);
    if (tailSize < 2 || !imageFile.seek(size - tailSize)) return false;

    uint8_t tail[TAIL_BYTES];
    const int readBytes = imageFile.read(tail, tailSize);
    for (int i = readBytes - 2; i >= 0; i--) {
      if (tail[i] == 0xFF && tail[i + 1] == 0xD9) return true;
    }
    return false;
  }

  if (format == ImageFormatDetector::Format::Png) {
    if (size < 12 || !imageFile.seek(size - 8)) return false;
    uint8_t chunkType[4] = {};
    return imageFile.read(chunkType, sizeof(chunkType)) == sizeof(chunkType) && memcmp(chunkType, "IEND", 4) == 0;
  }

  return format != ImageFormatDetector::Format::Unknown;
}

// How strongly an image's file name (its stem, lowercased) says it is the cover: 3 for the cover
// itself, 2 for a variant of it, 1 for a title page, 0 for anything else. A bare "title" does not
// count: it is a title banner at least as often as the cover. A back cover is no cover.
// After witchling-reader commit 5b62bbee (syfq91), which scores the names the same way.
int coverNameRank(const std::string_view stem) {
  for (const char* name :
       {"cover", "frontcover", "front-cover", "front_cover", "cover-image", "coverimage", "cover_image", "bookcover"}) {
    if (stem == name) return 3;
  }
  const auto endsWith = [&stem](const std::string_view s) {
    return stem.size() >= s.size() && stem.substr(stem.size() - s.size()) == s;
  };
  if (stem.find("back") == std::string_view::npos &&
      (stem.substr(0, 5) == "cover" || endsWith("_cover") || endsWith("-cover"))) {
    return 2;
  }
  for (const char* name : {"titlepage", "title-page", "title_page"}) {
    if (stem == name) return 1;
  }
  return 0;
}

// A markup document rather than a picture: an SVG wrapper (or an XHTML page) named as the cover.
bool isCoverWrapperDocument(const std::string& href) {
  for (const char* ext : {".svg", ".xhtml", ".html", ".htm"}) {
    if (FsHelpers::checkFileExtension(href, ext)) return true;
  }
  return false;
}

// A reference made inside a document, resolved against the document's own folder; empty for none.
std::string resolveInFolderOf(const std::string& docHref, const std::string_view ref) {
  if (ref.empty()) return {};
  const size_t lastSlash = docHref.rfind('/');
  std::string path = lastSlash == std::string::npos ? std::string{} : docHref.substr(0, lastSlash + 1);
  path.append(ref);
  return FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(path));
}

}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  cachePath = cachePathFor(this->filepath, cacheDir);
}

std::string Epub::cachePathFor(const std::string& filepath, const std::string& cacheDir) {
  // create a cache key based on the filepath
  return cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(filepath));
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, OpfCacheMode cacheMode) {
  const unsigned long opfParseStart = millis();
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }
  LOG_DBG("EBP", "content.opf size=%zu bytes", contentOpfSize);

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             cacheMode == OpfCacheMode::Enabled ? bookMetadataCache.get() : nullptr);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  bookMetadata.series = opfParser.series;
  bookMetadata.seriesIndex = opfParser.seriesIndex;
  bookMetadata.description = opfParser.description;
  bookMetadata.primaryAuthor = opfParser.primaryAuthor;
  bookMetadata.authorSort = opfParser.authorSort;
  bookMetadata.isbn = opfParser.isbn;
  bookMetadata.asin = opfParser.asin;

  // The manifest can name an SVG wrapper as the cover image instead of the picture itself:
  //
  //   <item properties="cover-image" media-type="image/svg+xml" href="cover.svg"/>
  //   cover.svg: <svg viewBox="0 0 1135 1600"><image xlink:href="cover.jpg"/></svg>
  //
  // Cover and thumbnail generation decode JPEG and PNG only, so the picture inside is the cover. A
  // wrapper that holds none is dropped, and the guide and ZIP-name scans below look for one instead.
  // Ported from crosspoint-reader PR #3539 (OSAKANA TARO, @osakanataro).
  if (isCoverWrapperDocument(bookMetadata.coverItemHref)) {
    std::string inWrapper = findCoverImageInDocument(bookMetadata.coverItemHref);
    if (inWrapper.empty()) {
      LOG_DBG("EBP", "Cover document holds no JPEG/PNG reference: %s", bookMetadata.coverItemHref.c_str());
    } else {
      LOG_DBG("EBP", "Cover document %s wraps %s", bookMetadata.coverItemHref.c_str(), inWrapper.c_str());
    }
    bookMetadata.coverItemHref = std::move(inWrapper);
  }

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // or if the manifest-declared cover path is invalid, try extracting the image
  // reference from the guide's cover page XHTML.
  bool shouldTryGuideCoverFallback = bookMetadata.coverItemHref.empty();
  if (!bookMetadata.coverItemHref.empty()) {
    size_t coverItemSize = 0;
    if (!getItemSize(bookMetadata.coverItemHref, &coverItemSize)) {
      LOG_DBG("EBP", "Manifest cover not found in archive, trying guide cover fallback: %s",
              bookMetadata.coverItemHref.c_str());
      shouldTryGuideCoverFallback = true;
    }
  }

  if (shouldTryGuideCoverFallback && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "Trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    std::string fromGuide = findCoverImageInDocument(opfParser.guideCoverPageHref);
    if (!fromGuide.empty()) {
      bookMetadata.coverItemHref = std::move(fromGuide);
      LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
    }
  }

  auto hasReadableCover = [&](const std::string& path) {
    if (path.empty()) return false;
    size_t coverSize = 0;
    return getItemSize(path, &coverSize);
  };

  if (!hasReadableCover(bookMetadata.coverItemHref)) {
    if (!bookMetadata.coverItemHref.empty()) {
      LOG_DBG("EBP", "Cover href unresolved, trying common cover candidates: %s", bookMetadata.coverItemHref.c_str());
      // Not a cover: the scan below takes a set href for one already found and would never look.
      bookMetadata.coverItemHref.clear();
    }

    // Single forward pass through the ZIP central directory. For each image entry, lowercase the
    // basename and rank it (coverNameRank); the best rank wins, and of one rank the first in the
    // archive. It used to be the first name known at all, so a title.png stored ahead of cover.png
    // was the cover. One SD scan regardless of EPUB size; a string is built only for a better rank.
    const unsigned long coverBatchStart = millis();
    int bestRank = 0;
    ZipFile zip(filepath);
    primeZip(zip);
    zip.streamCentralDirectoryNames([&](std::string_view path) {
      if (bestRank == 3) return;  // nothing ranks higher
      if (!FsHelpers::hasJpgExtension(path) && !FsHelpers::hasPngExtension(path)) return;

      // Extract and lowercase the basename (after last '/').
      const size_t slash = path.rfind('/');
      const std::string_view name = (slash != std::string_view::npos) ? path.substr(slash + 1) : path;
      const size_t dot = name.rfind('.');
      const std::string_view stem = (dot != std::string_view::npos) ? name.substr(0, dot) : name;

      // Lowercase the stem into a small stack buffer — stems are short.
      char lower[64];
      if (stem.size() >= sizeof(lower)) return;
      for (size_t i = 0; i < stem.size(); ++i)
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(stem[i])));
      lower[stem.size()] = '\0';

      const int rank = coverNameRank(std::string_view{lower, stem.size()});
      if (rank > bestRank) {
        bestRank = rank;
        bookMetadata.coverItemHref = std::string{path};
      }
    });

    if (bookMetadata.coverItemHref.empty()) {
      LOG_DBG("EBP", "Cover ZIP scan found no match (%lu ms)", millis() - coverBatchStart);
    } else {
      LOG_DBG("EBP", "Found cover image via ZIP scan (rank %d) in %lu ms: %s", bestRank, millis() - coverBatchStart,
              bookMetadata.coverItemHref.c_str());
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.pageMapPath.empty()) {
    pageMapItem = opfParser.pageMapPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "parseContentOpf total=%lu ms", millis() - opfParseStart);
  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

std::string Epub::findCoverImageInDocument(const std::string& docHref) const {
  // ~0.8 KB of fixed buffers, once per cover document scanned: on the heap, not the loop stack.
  const auto scanner = makeUniqueNoThrow<CoverImageRefScanner>();
  if (!scanner) {
    LOG_ERR("EBP", "No memory to scan cover document %s", docHref.c_str());
    return {};
  }
  if (!readItemContentsToStream(docHref, *scanner, 1024, scanner->stopFlag())) {
    LOG_DBG("EBP", "Could not read cover document %s", docHref.c_str());
  }
  const size_t count = scanner->candidateCount();
  if (count <= 1) return resolveInFolderOf(docHref, scanner->result());

  // Several pictures of the best kind: a cover page that shows the publisher's logo above the cover
  // art, say. The largest file is the cover. From witchling-reader commit 5b62bbee (syfq91).
  std::string best;
  size_t bestSize = 0;
  for (size_t i = 0; i < count; i++) {
    std::string path = resolveInFolderOf(docHref, scanner->candidate(i));
    size_t size = 0;
    if (getItemSize(path, &size) && (best.empty() || size > bestSize)) {
      best = std::move(path);
      bestSize = size;
    }
  }
  return best;
}

bool Epub::parseTocNcxFile(const bool writePageList) const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize = 0;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    if (writePageList) Storage.remove((getCachePath() + "/pagelist.bin").c_str());
    return false;
  }

  // Stream <pageList> entries straight to pagelist.bin (long printed-page lists used to
  // blow the X3 heap when accumulated in a std::vector — see PageListSink).
  std::optional<PageListSink> ncxPageListSink;
  if (writePageList) ncxPageListSink.emplace(getCachePath());
  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get(),
                         ncxPageListSink ? &*ncxPageListSink : nullptr);

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    if (ncxPageListSink) ncxPageListSink->discard();
    return false;
  }

  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not stream toc ncx data");
    if (ncxPageListSink) ncxPageListSink->discard();
    return false;
  }

  // Flush u16 count + close pagelist.bin (or remove it if no <pageList> entries were
  // streamed). The section builder later reads this file to stamp printed-page labels
  // onto rendered pages without re-parsing the NCX.
  if (ncxPageListSink) ncxPageListSink->finalize();

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize = 0;
  if (!getItemSize(tocNavItem, &navSize)) {
    Storage.remove((getCachePath() + "/pagelist.bin").c_str());
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  // Stream <nav epub:type="page-list"> entries straight to pagelist.bin (see PageListSink).
  PageListSink navPageListSink(getCachePath());
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get(), &navPageListSink);

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    navPageListSink.discard();
    return false;
  }

  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not stream toc nav data");
    navPageListSink.discard();
    return false;
  }

  // Flush u16 count + close pagelist.bin (or remove it if no entries were streamed).
  navPageListSink.finalize();

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

bool Epub::parsePageMapFile() const {
  // EPUB 2.01 page-map.xml: a separate top-level manifest item with media-type
  // "application/oebps-page-map+xml". Structure is a flat list of <page name="..." href="..."/>.
  if (pageMapItem.empty()) {
    LOG_DBG("EBP", "No page-map file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing page-map file: %s", pageMapItem.c_str());

  size_t pageMapSize = 0;
  if (!getItemSize(pageMapItem, &pageMapSize)) {
    Storage.remove((getCachePath() + "/pagelist.bin").c_str());
    return false;
  }

  // page-map hrefs are relative to the page-map file itself (typically content.opf's dir).
  const std::string pageMapBasePath = pageMapItem.substr(0, pageMapItem.find_last_of('/') + 1);
  // Stream page-map entries straight to pagelist.bin (see PageListSink).
  PageListSink pageMapPageListSink(getCachePath());
  PageMapParser pageMapParser(pageMapBasePath, pageMapSize, &pageMapPageListSink);

  if (!pageMapParser.setup()) {
    LOG_ERR("EBP", "Could not setup page-map parser");
    pageMapPageListSink.discard();
    return false;
  }

  if (!readItemContentsToStream(pageMapItem, pageMapParser, 1024)) {
    LOG_ERR("EBP", "Could not stream page-map data");
    pageMapPageListSink.discard();
    return false;
  }

  pageMapPageListSink.finalize();
  LOG_DBG("EBP", "Parsed page-map entries");
  return true;
}

void Epub::primeZip(ZipFile& zip) const {
  if (!zipDetailsCached_) return;
  ZipFile::ZipDetails d;
  d.centralDirOffset = zipCentralDirOffset_;
  d.totalEntries = zipTotalEntries_;
  d.isSet = true;
  zip.seedDetails(d);
}

void Epub::adoptZipDetails(const ZipFile& zip) const {
  const auto& d = zip.details();
  if (!d.isSet || zipDetailsCached_) return;
  zipCentralDirOffset_ = d.centralDirOffset;
  zipTotalEntries_ = d.totalEntries;
  zipDetailsCached_ = true;
}

bool Epub::computeZipFingerprint(uint64_t* out) const {
  if (!zipFingerprintComputed_) {
    ZipFile zip(filepath);
    primeZip(zip);
    zipFingerprintValid_ = zip.contentFingerprint(&zipFingerprint_);
    zipFingerprintComputed_ = true;
    adoptZipDetails(zip);
  }
  if (!zipFingerprintValid_) return false;
  *out = zipFingerprint_;
  return true;
}

namespace {
constexpr uint32_t FINGERPRINT_MAGIC = 0x31504658;  // "XFP1"
constexpr const char* FINGERPRINT_FILE = "/fingerprint.bin";
constexpr const char* TOC_RETRY_FILE = "/toc.retry";
// Builds that may end without the TOC: the first one, then a retry on each of two later opens.
constexpr uint8_t MAX_TOC_LOSSES = 3;
}  // namespace

uint8_t Epub::readTocLossCount() const {
  // A plain open, not openFileForRead: the sidecar is absent for almost every book, and
  // openFileForRead logs every miss.
  FsFile f = Storage.open((cachePath + TOC_RETRY_FILE).c_str());
  if (!f) return 0;
  uint8_t count = 0;
  if (f.read(&count, 1) != 1) count = 0;
  f.close();
  return count;
}

void Epub::writeTocLossCount(const uint8_t count) const {
  FsFile f;
  if (!Storage.openFileForWrite("EBP", cachePath + TOC_RETRY_FILE, f)) {
    LOG_ERR("EBP", "Could not write TOC retry sidecar");
    return;
  }
  f.write(&count, 1);
  f.close();
}

bool Epub::lostTocRetryDue() const {
  const uint8_t losses = readTocLossCount();
  return losses > 0 && losses < MAX_TOC_LOSSES;
}

bool Epub::readStoredFingerprint(uint64_t* out) const {
  FsFile f;
  if (!Storage.openFileForRead("EBP", cachePath + FINGERPRINT_FILE, f)) return false;
  uint32_t magic = 0;
  uint64_t fp = 0;
  const bool ok = f.read(&magic, sizeof(magic)) == sizeof(magic) && magic == FINGERPRINT_MAGIC &&
                  f.read(&fp, sizeof(fp)) == sizeof(fp);
  f.close();
  if (ok) *out = fp;
  return ok;
}

void Epub::writeStoredFingerprint(const uint64_t fp) const {
  FsFile f;
  if (!Storage.openFileForWrite("EBP", cachePath + FINGERPRINT_FILE, f)) {
    LOG_ERR("EBP", "Could not write fingerprint sidecar");
    return;
  }
  f.write(reinterpret_cast<const uint8_t*>(&FINGERPRINT_MAGIC), sizeof(FINGERPRINT_MAGIC));
  f.write(reinterpret_cast<const uint8_t*>(&fp), sizeof(fp));
  f.close();
}

bool Epub::clearCacheIfContentChanged() const {
  // The cache key is derived from the file path only, so identical path + different content would silently serve
  // a stale cache. fingerprint.bin pins the ZIP content fingerprint the cache was built from; a mismatch invalidates
  // the entire cache dir (sections, CSS, thumbs, the book's fonts -- all derived from the old bytes).
  uint64_t zipFp = 0;
  if (!computeZipFingerprint(&zipFp) || !BookMetadataCache::cacheExists(cachePath)) return false;
  uint64_t storedFp = 0;
  if (!readStoredFingerprint(&storedFp)) {
    // Pre-fingerprint cache (created before this firmware): adopt the current
    // content instead of forcing every existing book through a full rebuild.
    writeStoredFingerprint(zipFp);
    return false;
  }
  if (storedFp == zipFp) return false;
  LOG_INF("EBP", "Book content changed at same path, rebuilding cache");
  clearCache(false);
  return true;
}

bool Epub::needsFirstOpenIndexing() const {
  // Spine/TOC cache missing -> load() rebuilds it (content.opf + TOC/NCX + book.bin).
  if (!BookMetadataCache::cacheExists(cachePath)) return true;
  // Compiled CSS rules cache missing -> load() runs the (slow) CSS compile.
  if (!CssParser(cachePath).hasCache()) return true;
  // Book content changed at the same path -> load() wipes the cache and rebuilds.
  uint64_t zipFp = 0, storedFp = 0;
  if (computeZipFingerprint(&zipFp) && readStoredFingerprint(&storedFp) && storedFp != zipFp) return true;
  // An earlier build lost the TOC -> the reader's load() rebuilds to retry it.
  if (lostTocRetryDue()) return true;
  return false;
}

void Epub::loadImageManifest() {
  // Return immediately if already loaded (e.g. during this same session).
  if (imageManifest && imageManifest->isLoaded()) return;

  imageManifest.reset(new (std::nothrow) EpubImageManifest());
  if (!imageManifest) {
    LOG_ERR("EBP", "Failed to alloc EpubImageManifest");
    return;
  }
  // Loads images.bin if present, else starts an empty cache. Either way the manifest fills
  // incrementally via ensureResolved() as section indexing first encounters each image, so
  // there is no up-front whole-book header scan.
  imageManifest->load(cachePath);
}

void Epub::discoverCssFilesFromZip() {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot discover CSS from ZIP because book metadata cache is not loaded");
    return;
  }

  // Use streamCentralDirectoryNames (O(1) heap, fixed 256-byte stack buffer per entry)
  // instead of loadAllFileStatSlims(). The old path built a full unordered_map of every
  // ZIP entry — on EPUBs with 3000+ entries this consumed ~200 KB and crashed.
  ZipFile zf(filepath);
  primeZip(zf);

  const size_t lastSlash = contentBasePath.find_last_of('/');
  const std::string opfDir = (lastSlash != std::string::npos) ? contentBasePath.substr(0, lastSlash + 1) : "";

  if (!zf.streamCentralDirectoryNames([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) return;
        if (FsHelpers::hasCssExtension(filePath)) {
          if (std::find(cssFiles.begin(), cssFiles.end(), filePath) == cssFiles.end()) {
            LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
            cssFiles.push_back(std::string{filePath});
          }
        }
      })) {
    LOG_ERR("EBP", "Failed to stream ZIP central directory for CSS discovery");
  }
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  // Base heap for a compile-mode CSS parse (parser state + SD buffers + reserve); the per-file
  // gate adds the stylesheet's size on top. Replaces a flat 64 KB gate that rejected small
  // stylesheets whenever the reader opened with a resident framebuffer and modest free heap.
  constexpr size_t CSS_PARSE_BASE_HEAP_BYTES = 24 * 1024;

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // No cache yet - parse CSS files
  if (!cssParser->beginCacheCompile()) {
    LOG_ERR("EBP", "Failed to start CSS compile pipeline");
    return;
  }

  bool skippedForLowHeap = false;
  for (const auto& cssPath : cssFiles) {
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Dynamic heap gate: the compile-mode parse streams characters through fixed-size buffers and
    // stages rules to a temp SD file, so its heap need is a small base working set plus
    // per-selector bookkeeping that scales with the stylesheet — not the flat 64 KB the old gate
    // demanded (observed skipping a 3.3 KB stylesheet at 63.8 KB free, silently stripping the
    // book's styles). Base covers parser state + SD buffers + reserve; 1x file size generously
    // covers the selector-offset staging. Unknown size (lookup failed) gates on the base alone.
    const uint32_t freeHeap = ESP.getFreeHeap();
    const size_t neededHeap = CSS_PARSE_BASE_HEAP_BYTES + cssFileSize;
    if (freeHeap < neededHeap) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap, neededHeap,
              cssPath.c_str());
      skippedForLowHeap = true;
      continue;
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    FsFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    cssParser->setStylesheetPath(cssPath);  // its url()s resolve against its own directory
    const bool compiledOk = cssParser->appendCompiledFromStream(tempCssFile);
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
    if (!compiledOk) {
      // Once the compile pipeline fails (I/O error or MAX_RULES hit) it stays failed for the
      // rest of this session — endCacheCompile() will discard everything below — so parsing
      // further stylesheets is pure wasted SD I/O.
      LOG_ERR("EBP", "Failed to compile CSS file: %s, aborting CSS compile", cssPath.c_str());
      break;
    }
  }

  // A stylesheet skipped for low heap must NOT produce a persisted cache: an incomplete (or
  // empty) index loads as valid on every later open — hasCache() then short-circuits the
  // re-parse — so one low-heap open would permanently strip the book's styles (observed
  // on-device). Abort instead; with no cache on disk the next open re-parses, and that
  // missing-cache path also invalidates the section caches built unstyled this session.
  if (skippedForLowHeap) {
    cssParser->abortCacheCompile();
    LOG_ERR("EBP", "CSS cache not persisted (stylesheet skipped on low heap); will re-parse on next open");
    cssParser->clear();
    return;
  }

  // Finalize compact cache for next time.
  if (!cssParser->endCacheCompile()) {
    LOG_ERR("EBP", "Failed to finalize CSS rules cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files", cssParser->ruleCount(), cssFiles.size());
  cssParser->clear();
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss, BuildArena* scratch) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  // The lent region serves every stream read below; cleared on every way out of this function.
  struct ScratchScope {
    BuildArena*& slot;
    ~ScratchScope() { slot = nullptr; }
  } scratchScope{loadScratch_};
  loadScratch_ = (scratch != nullptr && scratch->valid()) ? scratch : nullptr;
  tocReliability = TocReliability::Unknown;

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  clearCacheIfContentChanged();

  // An earlier build lost the TOC: rebuild rather than reuse its book.bin, which has no room to
  // take the TOC in afterwards. Only on the reader's open (see setLostTocRetryEnabled()).
  const bool retryLostToc = buildIfMissing && lostTocRetryEnabled_ && lostTocRetryDue();

  // Try to load existing cache first
  if (!retryLostToc && bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        if (!parseContentOpf(bookMetadataCache->coreMetadata, OpfCacheMode::Disabled)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          // Handle case where CSS files are not listed in OPF manifest
          // but are still referenced by HTML files - discover and parse them too
          discoverCssFilesFromZip();
        }
        parseCssFiles();
        // Invalidate section caches so they are rebuilt with the new CSS
        removeSpineCaches();
      }
    }
    applyMetadataSidecar();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  if (retryLostToc) {
    LOG_INF("EBP", "Rebuilding spine/TOC cache to retry a TOC an earlier build lost");
  } else {
    LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  }
  setupCacheDir();

  // A retry is counted before it starts, so one that fails or takes the device down still uses
  // up an attempt.
  const uint8_t tocLossesBefore = readTocLossCount();
  const auto tocLossesAfter = static_cast<uint8_t>(std::min(tocLossesBefore + 1, 255));
  if (retryLostToc) writeTocLossCount(tocLossesAfter);

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata, OpfCacheMode::Enabled)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;
  bool navAttempted = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    navAttempted = true;
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available, or if the nav parsed but is sparse.
  // Converted books (e.g. Amazon exports) often pair a coarse EPUB 3 nav (parts and front matter
  // only) with an NCX carrying the full chapter list. A parsed nav whose spine-mapped entries cover
  // less than half the spine gives the NCX a chance, and the source with more mapped entries wins.
  // Raw counts would let a stale NCX full of hrefs that no longer resolve out-count a smaller,
  // working nav (see TocFallbackPolicy.h). Each re-parse starts from an empty TOC temp file, so the
  // loser's entries are discarded rather than appended to. Ported from crosspoint-reader PR #2603
  // (Ryan Jarvis, @Cabalist).
  const int navCount = bookMetadataCache->getTocCount();
  (void)navCount;  // only read by LOG_DBG, which compiles out at lower log levels
  const int navMappedCount = bookMetadataCache->getTocMappedCount();
  const int spineItemCount = bookMetadataCache->getSpineCount();
  const bool navSparse = toc_fallback::navIsSparse(tocParsed, navMappedCount, spineItemCount);
  if ((!tocParsed || navSparse) && !tocNcxItem.empty()) {
    if (navAttempted && !bookMetadataCache->resetTocPassOutput()) {
      LOG_ERR("EBP", "Could not reset TOC temp output before NCX fallback");
      return false;
    }
    if (navSparse) {
      LOG_DBG("EBP", "Nav TOC sparse (%d/%d entries mapped for %d spine items), trying NCX", navMappedCount, navCount,
              spineItemCount);
    } else {
      LOG_DBG("EBP", "Falling back to NCX TOC");
    }
    // The page list stays the nav's: an NCX tried against a parsed nav must not replace or remove
    // the nav's pagelist.bin.
    const bool ncxParsed = parseTocNcxFile(/*writePageList=*/!tocParsed);
    const int ncxMappedCount = bookMetadataCache->getTocMappedCount();
    if (toc_fallback::shouldAdoptNcx(tocParsed, navMappedCount, ncxParsed, ncxMappedCount)) {
      if (tocParsed) {
        LOG_DBG("EBP", "NCX TOC adopted (%d/%d entries mapped)", ncxMappedCount, bookMetadataCache->getTocCount());
      }
      tocParsed = true;
    } else if (tocParsed) {
      // The NCX failed or mapped no more entries: parse the nav again to restore its entries.
      LOG_DBG("EBP", "Keeping nav TOC (%d/%d entries mapped)", navMappedCount, navCount);
      if (!bookMetadataCache->resetTocPassOutput()) {
        LOG_ERR("EBP", "Could not reset TOC temp output to restore the nav TOC");
        return false;
      }
      tocParsed = parseTocNavFile();
    }
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  // Recorded before book.bin is written, so no book.bin without its TOC outlives the build
  // unrecorded. A book that declares no TOC has nothing to lose.
  const bool tocLost = !tocParsed && (!tocNavItem.empty() || !tocNcxItem.empty());
  if (tocLost && !retryLostToc) {
    writeTocLossCount(tocLossesAfter);
  } else if (!tocLost && tocLossesBefore > 0) {
    Storage.remove((cachePath + TOC_RETRY_FILE).c_str());
  }

  // EPUB 2.01 page-map.xml — only parse if neither NCX <pageList> nor nav page-list
  // wrote a pagelist.bin already (so an explicit NCX/nav printed-page list always wins).
  if (!pageMapItem.empty()) {
    const auto pageListPath = getCachePath() + "/pagelist.bin";
    if (!Storage.exists(pageListPath.c_str())) {
      parsePageMapFile();
    }
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  if (!skipLoadingCss) {
    // Handle case where CSS files are not listed in OPF manifest
    // but are still referenced by HTML files - discover and parse them too
    discoverCssFilesFromZip();
    // Parse CSS files after cache reload
    parseCssFiles();
  }
  // Sections laid out before this build are stale when the CSS was just recompiled, or when a
  // retry got the TOC back: its anchors start new pages, which also changes the page counts
  // pages.bin recorded while the book was read without it.
  const bool tocRecovered = retryLostToc && !tocLost;
  if (!skipLoadingCss || tocRecovered) removeSpineCaches();
  if (tocRecovered) SpinePageIndex::discard(cachePath);

  // Pin the content this cache was built from (see clearCacheIfContentChanged()).
  if (uint64_t zipFp = 0; computeZipFingerprint(&zipFp)) writeStoredFingerprint(zipFp);

  applyMetadataSidecar();
  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

static_assert(SidecarFiles::kMetadataStampBytes >= Epub::MAX_METADATA_SIDECAR_BYTES,
              "the recent-books staleness stamp must hash every sidecar byte applyMetadataSidecar() reads");

// Calibre writes an OPF beside each exported book. Where one sits next to the
// EPUB it is authoritative for that book's descriptive metadata, so a user can
// correct a title, author or series without rewriting the book - the same rule
// the cover sidecar already follows.
//
// Only the descriptive fields are taken. coverItemHref and textReferenceHref
// are zip-internal paths that a sidecar cannot meaningfully supply, and covers
// have their own sidecar already. Empty sidecar fields are skipped, so a
// partial sidecar cannot blank out good embedded metadata.
void Epub::applyMetadataSidecar() const {
  if (!bookMetadataCache) return;
  MetadataSidecarFields sidecar;
  if (MetadataSidecar::read(filepath, sidecar) != MetadataSidecar::Result::Read) return;

  auto& md = bookMetadataCache->coreMetadata;
  if (!sidecar.title.empty()) md.title = sidecar.title;
  if (!sidecar.author.empty()) md.author = sidecar.author;
  MetadataSidecar::overlayPrimaryAuthor(sidecar, md.primaryAuthor, md.authorSort);
  if (!sidecar.language.empty()) md.language = sidecar.language;
  if (!sidecar.series.empty()) md.series = sidecar.series;
  if (!sidecar.seriesIndex.empty()) md.seriesIndex = sidecar.seriesIndex;
  if (!sidecar.description.empty()) md.description = sidecar.description;
  if (!sidecar.isbn.empty()) md.isbn = sidecar.isbn;
  if (!sidecar.asin.empty()) md.asin = sidecar.asin;
  LOG_DBG("EBP", "Applied metadata sidecar for %s", filepath.c_str());
}

// One-entry memo for loadForCover().
//
// loadForCover() re-parses content.opf whenever book.bin is absent, and the home screen asks the
// SAME book for cover metadata three to five times in a row: once to try the thumb, once to check
// whether cover.img is cached, once to start the extract session, then once more per thumb size.
// Device-measured on X4, one Cyrillic book paid 5 x 301 ms and a five-book carousel spent ~3.3 s
// re-deriving answers it had just computed.
//
// One entry is all it takes, because those repeats are consecutive. Keyed by path AND file size,
// so a book replaced under the same name cannot serve a stale cover. Cleared by
// clearCoverMetadataMemo() when the burst ends; until then it is simply replaced as the carousel
// moves on, so at most one book's metadata is ever held.
namespace {
struct CoverMetadataMemo {
  std::string path;
  uint32_t size = 0;
  BookMetadataCache::BookMetadata meta;
  bool valid = false;
  // Whether the cover entry is STORED, and where. -1 = not yet asked. Memoized alongside the
  // metadata because answering it costs a central-directory scan, and the thumbnail path asks
  // once per size per attempt -- allocation churn immediately before the cover extract session
  // needs a large contiguous chunk, which is the last place to be fragmenting the heap.
  int8_t stored = -1;
  uint32_t storedOffset = 0;
  uint32_t storedSize = 0;
};
CoverMetadataMemo g_coverMemo;

// 0 when the book cannot be opened, which disables the memo for that call rather than risking a
// match on an unknown size.
uint32_t coverMemoKeySize(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("EBP", path, f)) return 0;
  const uint32_t size = static_cast<uint32_t>(f.size());
  f.close();
  return size;
}
}  // namespace

void Epub::clearCoverMetadataMemo() { g_coverMemo = CoverMetadataMemo{}; }

bool Epub::loadForCover(BuildArena* scratch) {
  coverKnownAbsent_ = false;
  struct ScratchScope {
    BuildArena*& slot;
    ~ScratchScope() { slot = nullptr; }
  } scratchScope{loadScratch_};
  loadScratch_ = (scratch != nullptr && scratch->valid()) ? scratch : nullptr;
  // Cover-only load: get coverItemHref WITHOUT building the spine/TOC book.bin (which, on a huge
  // book, is both slow and the site of the large manifest-index build). Used by RecentBooks / Home
  // cover thumbnails so showing a thumbnail never triggers a full-book parse.
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  cssParser.reset(new CssParser(cachePath));  // constructed for API symmetry; not parsed here

  // Fast path: a book.bin already exists — it carries coverItemHref, no OPF parse needed.
  if (bookMetadataCache->load()) {
    applyMetadataSidecar();
    return true;
  }

  // Second fast path: this book's OPF was parsed moments ago (see the memo note above).
  const uint32_t memoKey = coverMemoKeySize(filepath);
  if (memoKey != 0 && g_coverMemo.valid && g_coverMemo.size == memoKey && g_coverMemo.path == filepath) {
    bookMetadataCache->coreMetadata = g_coverMemo.meta;
    if (bookMetadataCache->coreMetadata.coverItemHref.empty()) {
      coverKnownAbsent_ = true;
      return false;
    }
    bookMetadataCache->markCoverMetadataLoaded();
    applyMetadataSidecar();
    return true;
  }

  // No cache: metadata-only OPF parse. OpfCacheMode::Disabled passes a null cache to ContentOpfParser,
  // so NO manifest item index / .items.bin / spine cache is built — only title/author/coverItemHref
  // are extracted. This is the whole point: a 1732-spine book contributes no giant index here.
  if (!parseContentOpf(bookMetadataCache->coreMetadata, OpfCacheMode::Disabled)) {
    LOG_INF("EBP", "loadForCover: content.opf parse failed for %s", filepath.c_str());
    return false;  // deliberately not memoized: a failed parse may be transient
  }
  // Memoized even when there is no cover: "this book has none" is exactly the answer the next
  // three calls would otherwise re-parse the OPF to rediscover.
  if (memoKey != 0) {
    // A fresh entry, not the old one edited: the stored-cover answer (stored / storedOffset /
    // storedSize) belongs to the book it was asked for. Keeping it handed this book the previous
    // book's cover location -- decoded at that offset in THIS file, rejected as an unsupported
    // image, and recorded as a book without a cover for good.
    g_coverMemo = CoverMetadataMemo{};
    g_coverMemo.path = filepath;
    g_coverMemo.size = memoKey;
    g_coverMemo.meta = bookMetadataCache->coreMetadata;
    g_coverMemo.valid = true;
  }
  if (bookMetadataCache->coreMetadata.coverItemHref.empty()) {
    coverKnownAbsent_ = true;
    return false;  // no discoverable cover — caller shows a placeholder
  }
  // Mark loaded so ensureCoverImageCached()'s isLoaded() gate passes. Spine/TOC stay empty — the
  // caller uses only the cover path (documented on loadForCover / markCoverMetadataLoaded).
  bookMetadataCache->markCoverMetadataLoaded();
  applyMetadataSidecar();
  return true;
}

bool Epub::loadForMetadata(BuildArena* scratch, const bool useBookBin) {
  // Metadata-only load: see the header for why this exists (issue #104 — the series-sequel scan used
  // to full-load every EPUB in the folder). Structured exactly like loadForCover(); the only
  // difference is which field the caller goes on to read, so neither marks the other's data valid.
  struct ScratchScope {
    BuildArena*& slot;
    ~ScratchScope() { slot = nullptr; }
  } scratchScope{loadScratch_};
  loadScratch_ = (scratch != nullptr && scratch->valid()) ? scratch : nullptr;
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  cssParser.reset(new CssParser(cachePath));  // constructed for API symmetry; not parsed here

  // Fast path: an existing book.bin already carries title/author/series, no OPF parse needed --
  // unless the caller wants what book.bin does not keep (the primary author and its file-as).
  if (useBookBin && bookMetadataCache->load()) {
    applyMetadataSidecar();
    return true;
  }

  // No cache: metadata-only OPF parse. OpfCacheMode::Disabled passes a null cache to
  // ContentOpfParser, so no manifest item index / .items.bin / spine cache is built.
  if (!parseContentOpf(bookMetadataCache->coreMetadata, OpfCacheMode::Disabled)) {
    LOG_INF("EBP", "loadForMetadata: content.opf parse failed for %s", filepath.c_str());
    return false;
  }
  // Mark loaded for API symmetry with loadForCover(); spine/TOC stay empty and must not be used.
  bookMetadataCache->markCoverMetadataLoaded();
  applyMetadataSidecar();
  return true;
}

bool Epub::clearCache(const bool preserveThumbs) const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!preserveThumbs) {
    if (!Storage.removeDir(cachePath.c_str())) {
      LOG_ERR("EPB", "Failed to clear cache");
      return false;
    }
    LOG_DBG("EPB", "Cache cleared successfully");
    return true;
  }

  // Delete the per-spine caches (bulk removal).
  removeSpineCaches();

  // Iterate the cache root and remove parsing artifacts, but preserve thumbnail
  // and cover BMPs so the home screen doesn't have to regenerate them (slow).
  FsFile dir = Storage.open(cachePath.c_str());
  if (!dir || !dir.isDirectory()) {
    LOG_ERR("EPB", "Failed to open cache dir for selective clear");
    if (dir) dir.close();
    return false;
  }

  char nameBuf[128];
  bool anyFailed = false;
  for (FsFile f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(nameBuf, sizeof(nameBuf));
    const bool isDir = f.isDirectory();
    f.close();

    const std::string name(nameBuf);
    // Keep thumbnail and cover BMPs, and the cached raw cover image — all are
    // expensive to regenerate (require ZIP decompression or format conversion).
    if (FsHelpers::hasBmpExtension(name) || name == "cover.img") continue;

    // A subdirectory (the image manifest's img/) goes as a whole, like sections/ above: a plain
    // file remove refuses a directory, which logged "Failed to remove cache file" on every clear
    // and left the directory behind.
    const std::string fullPath = cachePath + "/" + name;
    if (!(isDir ? Storage.removeDir(fullPath.c_str()) : Storage.remove(fullPath.c_str()))) {
      LOG_ERR("EPB", "Failed to remove cache %s: %s", isDir ? "directory" : "file", fullPath.c_str());
      anyFailed = true;
    }
  }
  dir.close();

  LOG_DBG("EPB", "Cache cleared successfully");
  return !anyFailed;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

std::string Epub::spineCacheRoot(const std::string& cachePath) { return cachePath + "/spines"; }

std::string Epub::spineCacheDir(const std::string& cachePath, const int spineIndex) {
  return spineCacheRoot(cachePath) + "/" + std::to_string(spineIndex / SPINE_CACHE_BUCKET_SIZE);
}

void Epub::removeSpineCaches() const {
  Storage.removeDir(spineCacheRoot(cachePath).c_str());
  // Nothing reads the old flat layout any more; it only goes when the caches are invalidated or
  // cleared anyway, rather than as a migration on book open: removing a directory deletes each
  // entry by path, which re-scans it every time and is quadratic in its size.
  const std::string legacy = cachePath + "/sections";
  if (Storage.exists(legacy.c_str())) {
    Storage.removeDir(legacy.c_str());
  }
}

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getPrimaryAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.primaryAuthor;
}

const std::string& Epub::getIsbn() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.isbn;
}

const std::string& Epub::getAsin() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.asin;
}

const std::string& Epub::getAuthorSort() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.authorSort;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

const std::string& Epub::getSeries() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.series;
}

const std::string& Epub::getSeriesIndex() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.seriesIndex;
}

const std::string& Epub::getDescription() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }
  return bookMetadataCache->coreMetadata.description;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

std::string Epub::getCoverImageCachePath() const { return cachePath + "/cover.img"; }

std::string Epub::getCoverItemHref() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return "";
  return bookMetadataCache->coreMetadata.coverItemHref;
}

bool Epub::coverImageCachedValidOnly() const {
  const auto coverCachePath = getCoverImageCachePath();
  if (!Storage.exists(coverCachePath.c_str())) return false;
  FsFile existing;
  if (!Storage.openFileForRead("EBP", coverCachePath, existing)) {
    existing.close();
    return true;  // can't open to validate — assume valid, let the decode fail if needed
  }
  const auto fmt = ImageFormatDetector::detect(existing);
  const bool complete = coverImageComplete(existing, fmt);
  existing.close();
  return complete;
}

bool Epub::coverImageCachedButUnsupported() const {
  const auto coverCachePath = getCoverImageCachePath();
  if (!Storage.exists(coverCachePath.c_str())) return false;
  FsFile existing;
  if (!Storage.openFileForRead("EBP", coverCachePath, existing)) {
    existing.close();
    return false;  // can't open to validate — treat as transient, not as a permanent verdict
  }
  const uint32_t size = existing.size();
  const auto fmt = ImageFormatDetector::detect(existing);
  existing.close();
  // ImageFormatDetector::detect sniffs the first 8 bytes only, and those are the first bytes the
  // sliced extractor writes — so a PARTIALLY extracted PNG/JPEG/GIF already sniffs correctly
  // and never reaches this verdict. Requiring the full 8 bytes is what keeps a mid-extraction
  // file (0..7 bytes on disk) from being condemned as undecodable.
  return size >= COVER_FORMAT_SNIFF_BYTES && fmt == ImageFormatDetector::Format::Unknown;
}

bool Epub::coverImageCachedAndValid(bool allowExtract) const {
  // Fast path: already-extracted and valid — no inflate needed in either mode.
  if (coverImageCachedValidOnly()) return true;
  // Not cached. Only the extracting mode may inflate it now; the non-extracting mode
  // defers to a sliced extractor so a multi-MB inflate never runs in one loop() tick.
  return allowExtract && ensureCoverImageCached();
}

bool Epub::ensureCoverImageCached() const {
  const auto coverCachePath = getCoverImageCachePath();
  if (Storage.exists(coverCachePath.c_str())) {
    FsFile existing;
    if (Storage.openFileForRead("EBP", coverCachePath, existing)) {
      const auto fmt = ImageFormatDetector::detect(existing);
      const bool complete = coverImageComplete(existing, fmt);
      existing.close();
      if (complete) return true;
    } else {
      existing.close();
      return true;  // can't open to validate — assume valid, let generateThumbBmp fail if needed
    }
    LOG_ERR("EBP", "Cached cover.img is incomplete or unsupported, deleting: %s", coverCachePath.c_str());
    Storage.remove(coverCachePath.c_str());
  }

  // Sidecar cover: a .jpg/.jpeg/.png/.bmp file alongside the EPUB takes priority
  // over the embedded cover image (same resolution, no ZIP decompression needed).
  const auto sep = filepath.find_last_of("/\\");
  const auto dot = filepath.rfind('.');
  if (dot != std::string::npos && (sep == std::string::npos || dot > sep)) {
    const std::string base = filepath.substr(0, dot);
    for (const char* ext : {".jpg", ".jpeg", ".png", ".bmp", ".JPG", ".JPEG", ".PNG", ".BMP"}) {
      const std::string candidate = base + ext;
      if (Storage.exists(candidate.c_str())) {
        LOG_DBG("EBP", "Using sidecar cover: %s", candidate.c_str());
        FsFile src;
        if (!Storage.openFileForRead("EBP", candidate, src)) break;
        FsFile dst;
        if (!Storage.openFileForWrite("EBP", coverCachePath, dst)) {
          src.close();
          break;
        }
        uint8_t buf[512];
        int n;
        while ((n = src.read(buf, sizeof(buf))) > 0) dst.write(buf, n);
        src.close();
        dst.close();
        if (Storage.exists(coverCachePath.c_str())) {
          LOG_DBG("EBP", "Sidecar cover cached: %s", coverCachePath.c_str());
          return true;
        }
        break;
      }
    }
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot extract cover image, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  FsFile coverFile;
  if (!Storage.openFileForWrite("EBP", coverCachePath, coverFile)) return false;

  if (!readItemContentsToStream(coverImageHref, coverFile, 1024)) {
    LOG_ERR("EBP", "Failed to read cover image from EPUB: %s", coverImageHref.c_str());
    coverFile.close();
    Storage.remove(coverCachePath.c_str());
    return false;
  }

  coverFile.close();

  if (!Storage.openFileForRead("EBP", coverCachePath, coverFile)) return false;
  const bool empty = coverFile.size() == 0;
  const auto fmt = empty ? ImageFormatDetector::Format::Unknown : ImageFormatDetector::detect(coverFile);
  coverFile.close();
  if (empty || fmt == ImageFormatDetector::Format::Unknown) {
    LOG_ERR("EBP", "Cover image %s: %s", empty ? "extracted as empty file" : "has unsupported format",
            coverImageHref.c_str());
    Storage.remove(coverCachePath.c_str());
    return false;
  }

  LOG_DBG("EBP", "Cover image cached: %s", coverCachePath.c_str());
  return true;
}

namespace {
// A cover/thumbnail BMP whose write was interrupted (reboot/abort mid-decode) is left truncated:
// the header is intact but the pixel rows are short, so it passes a size>0 / exists check yet
// cannot be drawn (GFX "Failed to read row N"). Reuse a cached BMP only if it actually holds all
// its declared rows; otherwise the caller must regenerate it. Returns false for a 0-byte sentinel
// too (callers handle that separately as a permanent-failure marker).
bool coverBmpComplete(const std::string& path) {
  FsFile f;
  if (!Storage.openFileForRead("EBP", path, f)) return false;
  if (f.size() == 0) {
    f.close();
    return false;
  }
  Bitmap bmp(f);
  // Also require 8 bpp: covers cached by an earlier firmware are 2-bit, which still
  // parses fine but has already been quantized to four levels. Treating those as
  // valid would pin every existing book to the old format forever, so reject them
  // here and let the caller regenerate once.
  const bool ok = bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete() && bmp.getBpp() == 8;
  f.close();
  return ok;
}

// A thumbnail worth keeping: every pixel row present, and no larger than its slot (the themes draw
// thumbs 1:1; an oversized one from an older crop mode would be rescaled, aliasing its dither).
// The generators' own "already done" short-circuit. It used coverBmpComplete() until that learned
// to demand 8 bpp for the sleep-screen cover (ee71b96c8, 2026-08-05), which no 1-bit thumbnail
// passes: every generator call re-decoded a thumbnail that was already there. Mirrors
// ReaderActivity::isCoverThumbComplete, which the callers check first.
bool thumbBmpComplete(const std::string& path, const int slotWidth, const int slotHeight) {
  FsFile f;
  if (!Storage.openFileForRead("EBP", path, f)) return false;
  if (f.size() == 0) {
    f.close();
    return false;
  }
  Bitmap bmp(f);
  bool ok = bmp.parseHeaders() == BmpReaderError::Ok && bmp.isComplete();
  if (ok && slotWidth > 0 && slotHeight > 0 && (bmp.getWidth() > slotWidth || bmp.getHeight() > slotHeight)) {
    ok = false;
  }
  f.close();
  return ok;
}
}  // namespace

bool Epub::coverBmpReady(bool cropped) const { return coverBmpComplete(getCoverBmpPath(cropped)); }

bool Epub::generateCoverBmp(bool cropped) const {
  // Reuse only a COMPLETE cached BMP; a truncated one (interrupted write) must be regenerated.
  if (coverBmpReady(cropped)) return true;
  Storage.remove(getCoverBmpPath(cropped).c_str());  // drop any partial before regenerating

  if (!ensureCoverImageCached()) return false;

  const auto coverCachePath = getCoverImageCachePath();
  FsFile coverImage;
  if (!Storage.openFileForRead("EBP", coverCachePath, coverImage)) return false;

  const auto detectedFormat = ImageFormatDetector::detect(coverImage);
  if (detectedFormat == ImageFormatDetector::Format::Jpeg) {
    LOG_DBG("EBP", "Generating BMP from JPEG cover image (%s mode)", cropped ? "cropped" : "fit");
  } else if (detectedFormat == ImageFormatDetector::Format::Png) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
  } else {
    LOG_ERR("EBP", "Cover image has unsupported format");
    coverImage.close();
    return false;
  }

  FsFile coverBmp;
  if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
    coverImage.close();
    return false;
  }

  // 8-bit: this cover is drawn only by the sleep screen, which dithers at draw time
  // and can apply the adaptive tone filter. Quantizing to 4 levels here would throw
  // away the tonal range that filter needs. Thumbnails stay 1-bit -- they are drawn
  // on the home screen's BW-plane path and are a different artifact entirely.
  bool success = false;
  if (detectedFormat == ImageFormatDetector::Format::Jpeg) {
    success = JpegToBmpConverter::jpegFileToBmpStream(coverImage, coverBmp, cropped, /*grayscale8Bit=*/true);
  } else {
    success = PngToBmpConverter::pngFileToBmpStream(coverImage, coverBmp, cropped, /*grayscale8Bit=*/true);
  }

  coverImage.close();
  coverBmp.close();

  if (!success) {
    LOG_ERR("EBP", "Failed to generate BMP from cover image");
    Storage.remove(getCoverBmpPath(cropped).c_str());
  }

  LOG_DBG("EBP", "Generated BMP from cover image, success: %s", success ? "yes" : "no");
  return success;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }
std::string Epub::getThumbBmpPath(int width, int height) const {
  return cachePath + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
}

void Epub::writeThumbSentinel(const std::string& thumbPath) {
  FsFile thumbBmp;
  Storage.openFileForWrite("EBP", thumbPath, thumbBmp);
  thumbBmp.close();
}

bool Epub::openStoredCoverInPlace(FsFile& out, uint32_t* offset) const {
  const std::string href = getCoverItemHref();
  if (href.empty() || !offset) return false;

  // Ask the archive at most once per book (see CoverMetadataMemo::stored).
  const bool memoUsable = g_coverMemo.valid && g_coverMemo.path == filepath;
  uint32_t size = 0;
  if (memoUsable && g_coverMemo.stored >= 0) {
    if (g_coverMemo.stored == 0) return false;
    *offset = g_coverMemo.storedOffset;
    size = g_coverMemo.storedSize;
  } else {
    const bool stored = getStoredItemRange(href, offset, &size) && size != 0;
    if (memoUsable) {
      g_coverMemo.stored = stored ? 1 : 0;
      g_coverMemo.storedOffset = *offset;
      g_coverMemo.storedSize = size;
    }
    if (!stored) return false;
  }
  if (!Storage.openFileForRead("EBP", filepath, out)) return false;
  if (!out.seek(*offset)) {
    out.close();
    return false;
  }
  LOG_DBG("EBP", "Cover is stored: decoding in place, no extraction (%u bytes at %u)", static_cast<unsigned>(size),
          static_cast<unsigned>(*offset));
  return true;
}

ThumbResult Epub::generateThumbBmp(int height, bool allowExtract, BuildArena* scratch) const {
  {
    FsFile existing;
    if (Storage.openFileForRead("EBP", getThumbBmpPath(height), existing)) {
      const uint32_t sz = existing.size();
      existing.close();
      if (sz == 0) {  // 0-byte sentinel — a prior pass proved this cover structurally absent.
        LOG_DBG("EBP", "Sentinel found for h=%d thumb, skipping retry", height);
        return ThumbResult::StructurallyAbsent;
      }
      // size>0 is not "done": a thumb truncated by an interrupted write must be regenerated, not
      // returned as valid (the caller's completeness check would otherwise reject it forever and
      // loop). Reuse only a complete BMP; else fall through (openFileForWrite below truncates it).
      if (thumbBmpComplete(getThumbBmpPath(height), height * 6 / 10, height)) return ThumbResult::Ok;
      LOG_DBG("EBP", "Existing h=%d thumb is truncated — regenerating", height);
    }
  }

  // No cover item at all is structural — record a sentinel now regardless of allowExtract,
  // since no amount of extraction or retry can conjure a cover that the OPF doesn't declare.
  if (getCoverItemHref().empty()) {
    LOG_DBG("EBP", "No cover item for h=%d — writing structural sentinel", height);
    writeThumbSentinel(getThumbBmpPath(height));
    return ThumbResult::StructurallyAbsent;
  }

  // An extracted-but-undecodable cover is structural, and must be answered BEFORE the
  // transient bail below: coverImageCachedAndValid() reports false for it exactly as it does
  // for a not-yet-extracted cover, so without this the caller re-runs the extractor, gets the
  // same bytes, and defers again — forever (observed: a .png that is really an AVIF, re-inflated
  // from the ZIP every ~1.3 s). The unsupported-format branch further down never sees it.
  if (coverImageCachedButUnsupported()) {
    LOG_ERR("EBP", "Cached cover.img is not a supported format for h=%d — writing structural sentinel", height);
    writeThumbSentinel(getThumbBmpPath(height));
    return ThumbResult::StructurallyAbsent;
  }

  // Same in-place shortcut as the (width, height) overload above: a stored cover needs no
  // extraction at all.
  FsFile coverImage;
  uint32_t coverBase = 0;
  if (coverImageCachedAndValid(allowExtract)) {
    if (!Storage.openFileForRead("EBP", getCoverImageCachePath(), coverImage)) return ThumbResult::TransientFail;
  } else if (!openStoredCoverInPlace(coverImage, &coverBase)) {
    // Neither cached nor stored. No sentinel — let the sliced extractor produce it, or retry
    // next pass/boot. The caller counts these.
    LOG_DBG("EBP", "cover.img not cached/valid for h=%d — transient, deferring", height);
    return ThumbResult::TransientFail;
  }

  const auto detectedFormat = ImageFormatDetector::detect(coverImage, coverBase);
  if (detectedFormat == ImageFormatDetector::Format::Unknown) {
    // Cover extracted but its format is unsupported — structural, re-extraction yields the same
    // bytes. Sentinel so we stop trying.
    LOG_ERR("EBP", "Cached cover image is not a supported format — writing structural sentinel");
    coverImage.close();
    writeThumbSentinel(getThumbBmpPath(height));
    return ThumbResult::StructurallyAbsent;
  }

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
    coverImage.close();
    return ThumbResult::TransientFail;
  }

  const int thumbW = static_cast<int>(height * 0.6f);
  bool success = false;
  if (detectedFormat == ImageFormatDetector::Format::Jpeg) {
    LOG_DBG("EBP", "Generating thumb BMP from JPEG cover image");
    success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, thumbW, height, scratch);
  } else {
    LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
    success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, thumbW, height);
  }

  coverImage.close();
  thumbBmp.close();

  if (!success) {
    // Whether aborted for input or a plain decode failure, this is transient: drop the partial
    // thumb (never leave it as a false sentinel) and let the caller retry / count it.
    LOG_DBG("EBP", "Thumb decode for h=%d did not complete — removing partial, transient", height);
    Storage.remove(getThumbBmpPath(height).c_str());
    return ThumbResult::TransientFail;
  }
  LOG_DBG("EBP", "Generated thumb BMP from cover image (h=%d)", height);
  return ThumbResult::Ok;
}

ThumbResult Epub::generateThumbBmp(int width, int height, bool allowExtract, BuildArena* scratch,
                                   const bool crop) const {
  {
    FsFile existing;
    if (Storage.openFileForRead("EBP", getThumbBmpPath(width, height), existing)) {
      const uint32_t sz = existing.size();
      existing.close();
      if (sz == 0) {  // 0-byte sentinel — a prior pass proved this cover structurally absent.
        LOG_DBG("EBP", "Sentinel found for %dx%d thumb, skipping retry", width, height);
        return ThumbResult::StructurallyAbsent;
      }
      // size>0 is not "done": a thumb truncated by an interrupted write must be regenerated, not
      // returned as valid (the caller's completeness check would otherwise reject it forever and
      // loop). Reuse only a complete BMP; else fall through (openFileForWrite below truncates it).
      if (thumbBmpComplete(getThumbBmpPath(width, height), width, height)) return ThumbResult::Ok;
      LOG_DBG("EBP", "Existing %dx%d thumb is truncated — regenerating", width, height);
    }
  }

  // No cover item at all is structural — record a sentinel now regardless of allowExtract,
  // since no amount of extraction or retry can conjure a cover that the OPF doesn't declare.
  if (getCoverItemHref().empty()) {
    LOG_DBG("EBP", "No cover item for %dx%d — writing structural sentinel", width, height);
    writeThumbSentinel(getThumbBmpPath(width, height));
    return ThumbResult::StructurallyAbsent;
  }

  // See the h= overload above: extracted-but-undecodable must be answered before the transient
  // bail, or the extractor is re-run forever on bytes that can never decode.
  if (coverImageCachedButUnsupported()) {
    LOG_ERR("EBP", "Cached cover.img is not a supported format for %dx%d — writing structural sentinel", width, height);
    writeThumbSentinel(getThumbBmpPath(width, height));
    return ThumbResult::StructurallyAbsent;
  }

  // Prefer the extracted cover.img when we have it; otherwise the cover can still be decoded
  // straight out of the archive if the ZIP stores it uncompressed, which skips the extraction
  // entirely. Measured on X4: four carousel covers spent ~6.0 s being copied to cover.img before
  // any of them could be decoded.
  FsFile coverImage;
  uint32_t coverBase = 0;
  if (coverImageCachedAndValid(allowExtract)) {
    if (!Storage.openFileForRead("EBP", getCoverImageCachePath(), coverImage)) return ThumbResult::TransientFail;
  } else if (!openStoredCoverInPlace(coverImage, &coverBase)) {
    // Neither cached nor stored (a deflated entry has to be inflated first). No sentinel — let
    // the sliced extractor produce it, or retry next pass/boot. The caller counts these.
    LOG_DBG("EBP", "cover.img not cached/valid for %dx%d — transient, deferring", width, height);
    return ThumbResult::TransientFail;
  }

  const auto detectedFormat = ImageFormatDetector::detect(coverImage, coverBase);
  if (detectedFormat == ImageFormatDetector::Format::Unknown) {
    // Cover extracted but its format is unsupported — structural, re-extraction yields the same
    // bytes. Sentinel so we stop trying.
    LOG_ERR("EBP", "Cached cover image is not a supported format — writing structural sentinel");
    coverImage.close();
    writeThumbSentinel(getThumbBmpPath(width, height));
    return ThumbResult::StructurallyAbsent;
  }

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("EBP", getThumbBmpPath(width, height), thumbBmp)) {
    coverImage.close();
    return ThumbResult::TransientFail;
  }

  bool success = false;
  if (detectedFormat == ImageFormatDetector::Format::Jpeg) {
    LOG_DBG("EBP", "Generating %dx%d thumb BMP from JPEG cover image", width, height);
    success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, width, height, scratch, crop);
  } else {
    LOG_DBG("EBP", "Generating %dx%d thumb BMP from PNG cover image", width, height);
    success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, width, height, crop);
  }

  coverImage.close();
  thumbBmp.close();

  if (!success) {
    // Whether aborted for input or a plain decode failure, this is transient: drop the partial
    // thumb (never leave it as a false sentinel) and let the caller retry / count it.
    LOG_DBG("EBP", "Thumb decode for %dx%d did not complete — removing partial, transient", width, height);
    Storage.remove(getThumbBmpPath(width, height).c_str());
    return ThumbResult::TransientFail;
  }
  LOG_DBG("EBP", "Generated %dx%d thumb BMP from cover image", width, height);
  return ThumbResult::Ok;
}

ThumbResult Epub::generateThumbBmps(const std::pair<int, int>* sizes, const int count, const bool allowExtract,
                                    BuildArena* scratch) const {
  constexpr int kMax = JpegToBmpConverter::kMaxTargets;
  if (sizes == nullptr || count < 1 || count > kMax) return ThumbResult::TransientFail;

  // Which sizes still need a BMP. A sentinel on any of them answers for the cover as a whole: it is
  // written only when the cover itself is absent or undecodable, which no size can change.
  bool needed[kMax] = {};
  int neededCount = 0;
  for (int i = 0; i < count; ++i) {
    const std::string path = getThumbBmpPath(sizes[i].first, sizes[i].second);
    FsFile existing;
    if (Storage.openFileForRead("EBP", path, existing)) {
      const uint32_t sz = existing.size();
      existing.close();
      if (sz == 0) {
        LOG_DBG("EBP", "Sentinel found for %dx%d thumb, skipping retry", sizes[i].first, sizes[i].second);
        return ThumbResult::StructurallyAbsent;
      }
      if (thumbBmpComplete(path, sizes[i].first, sizes[i].second)) continue;
      LOG_DBG("EBP", "Existing %dx%d thumb is truncated — regenerating", sizes[i].first, sizes[i].second);
    }
    needed[i] = true;
    ++neededCount;
  }
  if (neededCount == 0) return ThumbResult::Ok;
  const auto sentinelAll = [&] {
    for (int i = 0; i < count; ++i) {
      if (needed[i]) writeThumbSentinel(getThumbBmpPath(sizes[i].first, sizes[i].second));
    }
  };

  // The same structural answers as the single-size overload, before the transient bail.
  if (getCoverItemHref().empty()) {
    LOG_DBG("EBP", "No cover item — writing structural sentinels for %d thumb size(s)", neededCount);
    sentinelAll();
    return ThumbResult::StructurallyAbsent;
  }
  if (coverImageCachedButUnsupported()) {
    LOG_ERR("EBP", "Cached cover.img is not a supported format — writing structural sentinels");
    sentinelAll();
    return ThumbResult::StructurallyAbsent;
  }

  FsFile coverImage;
  uint32_t coverBase = 0;
  if (coverImageCachedAndValid(allowExtract)) {
    if (!Storage.openFileForRead("EBP", getCoverImageCachePath(), coverImage)) return ThumbResult::TransientFail;
  } else if (!openStoredCoverInPlace(coverImage, &coverBase)) {
    LOG_DBG("EBP", "cover.img not cached/valid for %d thumb size(s) — transient, deferring", neededCount);
    return ThumbResult::TransientFail;
  }

  const auto detectedFormat = ImageFormatDetector::detect(coverImage, coverBase);
  if (detectedFormat == ImageFormatDetector::Format::Unknown) {
    LOG_ERR("EBP", "Cached cover image is not a supported format — writing structural sentinels");
    coverImage.close();
    sentinelAll();
    return ThumbResult::StructurallyAbsent;
  }

  // Open every output before decoding. Opening for write truncates, and a 0-byte thumb reads as a
  // "no cover" sentinel, so whatever was opened is removed again on any way out that fails.
  FsFile thumbs[kMax];
  const auto removeOutputs = [&] {
    for (int i = 0; i < count; ++i) {
      if (!needed[i]) continue;
      if (thumbs[i]) thumbs[i].close();
      Storage.remove(getThumbBmpPath(sizes[i].first, sizes[i].second).c_str());
    }
  };
  for (int i = 0; i < count; ++i) {
    if (needed[i] && !Storage.openFileForWrite("EBP", getThumbBmpPath(sizes[i].first, sizes[i].second), thumbs[i])) {
      coverImage.close();
      removeOutputs();
      return ThumbResult::TransientFail;
    }
  }

  bool success = false;
  if (detectedFormat == ImageFormatDetector::Format::Jpeg) {
    JpegToBmpConverter::BmpTarget targets[kMax];
    int n = 0;
    for (int i = 0; i < count; ++i) {
      if (needed[i]) targets[n++] = JpegToBmpConverter::BmpTarget{&thumbs[i], sizes[i].first, sizes[i].second};
    }
    LOG_DBG("EBP", "Generating %d thumb BMP(s) from one JPEG cover decode", n);
    success = JpegToBmpConverter::jpegFileTo1BitBmpStreamsWithSizes(coverImage, targets, n, scratch);
  } else {
    // PNG: one conversion per size, each from the start of the image.
    success = true;
    for (int i = 0; i < count && success; ++i) {
      if (!needed[i]) continue;
      LOG_DBG("EBP", "Generating %dx%d thumb BMP from PNG cover image", sizes[i].first, sizes[i].second);
      success = coverImage.seek(coverBase) && PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(
                                                  coverImage, thumbs[i], sizes[i].first, sizes[i].second);
    }
  }
  coverImage.close();

  if (!success) {
    // Whether aborted for input or a plain decode failure, this is transient: drop every partial
    // thumb (never leave one as a false sentinel) and let the caller retry / count it.
    LOG_DBG("EBP", "Thumb decode for %d size(s) did not complete — removing partials, transient", neededCount);
    removeOutputs();
    return ThumbResult::TransientFail;
  }
  for (int i = 0; i < count; ++i) {
    if (needed[i]) thumbs[i].close();
  }
  LOG_DBG("EBP", "Generated %d thumb BMP(s) from one cover decode", neededCount);
  return ThumbResult::Ok;
}

std::unique_ptr<CoverThumbSession> Epub::beginThumbSession(const std::pair<int, int>* sizes, const int count,
                                                           BuildArena* scratch) const {
  constexpr int kMax = JpegToBmpConverter::kMaxTargets;
  if (sizes == nullptr || count < 1 || count > kMax) return nullptr;

  // The same checks generateThumbBmps makes first, minus the answers it writes: any sentinel, or
  // every size already complete, is left to it.
  bool needed[kMax] = {};
  int neededCount = 0;
  for (int i = 0; i < count; ++i) {
    const std::string path = getThumbBmpPath(sizes[i].first, sizes[i].second);
    FsFile existing;
    if (Storage.openFileForRead("EBP", path, existing)) {
      const uint32_t sz = existing.size();
      existing.close();
      if (sz == 0) return nullptr;
      if (thumbBmpComplete(path, sizes[i].first, sizes[i].second)) continue;
    }
    needed[i] = true;
    ++neededCount;
  }
  if (neededCount == 0 || getCoverItemHref().empty() || coverImageCachedButUnsupported()) return nullptr;

  std::unique_ptr<CoverThumbSession> session(new (std::nothrow) CoverThumbSession());
  if (!session) return nullptr;
  uint32_t coverBase = 0;
  if (coverImageCachedAndValid(/*allowExtract=*/false)) {
    if (!Storage.openFileForRead("EBP", getCoverImageCachePath(), session->cover_)) return nullptr;
  } else if (!openStoredCoverInPlace(session->cover_, &coverBase)) {
    return nullptr;
  }
  if (ImageFormatDetector::detect(session->cover_, coverBase) != ImageFormatDetector::Format::Jpeg) return nullptr;

  JpegToBmpConverter::BmpTarget targets[kMax];
  for (int i = 0; i < count; ++i) {
    if (!needed[i]) continue;
    const int n = session->count_;
    session->paths_[n] = getThumbBmpPath(sizes[i].first, sizes[i].second);
    // Opening for write truncates; from here on the session removes this path unless it finishes.
    session->count_ = n + 1;
    if (!Storage.openFileForWrite("EBP", session->paths_[n], session->thumbs_[n])) return nullptr;
    targets[n] = JpegToBmpConverter::BmpTarget{&session->thumbs_[n], sizes[i].first, sizes[i].second};
  }
  session->jpeg_ = JpegThumbSession::begin(session->cover_, targets, session->count_, scratch);
  if (!session->jpeg_) return nullptr;
  LOG_DBG("EBP", "Sliced %s cover conversion for %d thumb size(s)",
          session->jpeg_->progressive() ? "progressive" : "baseline", session->count_);
  return session;
}

bool CoverThumbSession::progressive() const { return jpeg_ != nullptr && jpeg_->progressive(); }

CoverThumbSession::Status CoverThumbSession::continueSteps(const uint16_t units) {
  if (status_ != Status::Running) return status_;
  if (!jpeg_) {
    status_ = Status::Error;
    removeOutputs();
    return status_;
  }
  const auto st = jpeg_->continueSteps(units);
  if (st == JpegThumbSession::Status::Running) return Status::Running;
  jpeg_.reset();  // flushes (a no-op once Done) before the files close
  if (st == JpegThumbSession::Status::Done) {
    for (int i = 0; i < count_; ++i) thumbs_[i].close();
    cover_.close();
    status_ = Status::Done;
    LOG_DBG("EBP", "Generated %d thumb BMP(s) from one sliced cover decode", count_);
  } else {
    status_ = Status::Error;
    removeOutputs();
    LOG_DBG("EBP", "Sliced thumb decode for %d size(s) failed — removed partials, transient", count_);
  }
  return status_;
}

void CoverThumbSession::removeOutputs() {
  jpeg_.reset();
  for (int i = 0; i < count_; ++i) {
    if (thumbs_[i]) thumbs_[i].close();
    Storage.remove(paths_[i].c_str());
  }
  if (cover_) cover_.close();
}

CoverThumbSession::~CoverThumbSession() {
  if (status_ != Status::Done) {
    removeOutputs();
  } else if (cover_) {
    cover_.close();
  }
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  ZipFile zip(filepath);
  primeZip(zip);
  const auto content = zip.readFileToMemory(path.c_str(), size, trailingNullByte);
  adoptZipDetails(zip);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize, const bool* stop,
                                    BuildArena* scratch) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  // A lent region (the caller's, or the one lent to load()) hosts the read buffer and the inflate
  // ring when it has room for the largest ring; the decision is made before any byte reaches
  // `out`, so a refusal falls through to the heap path cleanly. A missing entry fails there
  // again, at negligible cost.
  BuildArena* const arena = scratch != nullptr ? scratch : loadScratch_;
  if (arena != nullptr && arena->valid()) {
    const size_t wanted = 1024 + InflateReader::ringSizeFor(0) + 2 * alignof(std::max_align_t);
    if (arena->capacity() - arena->used() >= wanted) {
      return readItemContentsToStreamWithArena(itemHref, out, arena, stop);
    }
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  ZipFile zip(filepath);
  primeZip(zip);
  const bool ok = zip.readFileToStream(path.c_str(), out, chunkSize, stop);
  adoptZipDetails(zip);
  return ok;
}

size_t Epub::readItemHeaderBytes(const std::string& itemHref, uint8_t* outBuf, const size_t maxBytes) const {
  if (itemHref.empty() || !outBuf || maxBytes == 0) return 0;
  const std::string path = FsHelpers::normalisePath(itemHref);
  ZipFile zip(filepath);
  primeZip(zip);
  const size_t got = zip.readBytesFromEntry(path.c_str(), outBuf, maxBytes);
  adoptZipDetails(zip);
  return got;
}

bool Epub::readItemContentsToStreamWithArena(const std::string& itemHref, Print& out, BuildArena* arena,
                                             const bool* stop) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  ZipFile zip(filepath);
  primeZip(zip);

  // EntryReader is the only deflate path in ZipFile that can take an arena; readFileToStream
  // always mallocs. Same 1 KB chunk size as that path, so the SD write pattern is unchanged.
  ZipFile::EntryReader reader(zip, 1024, arena);
  const bool opened = reader.open(path.c_str());
  adoptZipDetails(zip);
  if (!opened) return false;

  // Staging buffer on the stack rather than from the arena: the reader holds an arena block
  // until close(), and a nested block would have to be released before it (BuildArena is LIFO).
  // 512 B is nothing against the ~8 KB task stack, and this runs once per image, ever.
  uint8_t buffer[512];
  bool done = false;
  while (!done) {
    size_t produced = 0;
    if (!reader.step(buffer, sizeof(buffer), &produced, &done)) {
      LOG_ERR("EBP", "Arena extract failed mid-stream: %s", path.c_str());
      return false;
    }
    if (produced > 0 && out.write(buffer, produced) != produced) {
      LOG_ERR("EBP", "Failed to write all extracted bytes: %s", path.c_str());
      return false;
    }
    if (stop && *stop) return true;
  }
  return true;
}

namespace {

// Buffers the extract's writes on their way to SD.
//
// Both stream paths below hand the sink 512 B - 1 KB at a time, which is how they read; written
// straight through, a cover-sized entry becomes thousands of single-sector SD writes. Device-
// measured on X4: 857182 bytes took ~17 s that way, ~50 KB/s, while the SAME file read back by
// the PNG decoder through its 2 KB buffer managed ~215 KB/s. Batching into EXTRACT_WRITE_BUFFER_
// BYTES lets SdFat issue multi-sector transfers instead.
//
// A null/failed buffer degrades to pass-through rather than failing the extract — slow is a
// nuisance, no cover is a bug.
class BufferedExtractSink : public Print {
 public:
  BufferedExtractSink(FsFile& file, uint8_t* buffer, const size_t capacity)
      : writer_(file, buffer, buffer ? capacity : 0) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* data, const size_t size) override { return writer_.write(data, size) ? size : 0; }

  // Named drain() rather than flush(): Print::flush() is virtual with a void return, and this
  // one's result decides whether the extract succeeded.
  bool drain() { return writer_.flush(); }

 private:
  serialization::BufferedFileWriter writer_;
};

}  // namespace

bool Epub::extractItemToFileOnce(const std::string& itemHref, const std::string& destPath, BuildArena* arena) const {
  // Written under a temporary name and renamed once complete, so a reset or power loss mid-way
  // never leaves a short file under the real name. Consumers trust an existing extract as-is
  // (ImageBlock::ensureExtracted), and a truncated JPEG read as "no SOF marker" on every visit
  // of its page -- an image lost until someone deleted the file by hand (X3 2026-09-25).
  const std::string partPath = destPath + ".part";
  FsFile destFile;
  if (!Storage.openFileForWrite("EBP", partPath, destFile)) {
    LOG_ERR("EBP", "Failed to open dest for extract: %s", partPath.c_str());
    return false;
  }
  // The write buffer is reserved BEFORE the reader takes its own block, because BuildArena is
  // LIFO: the reader is created and released inside the call below, so it must sit on top of
  // this one. Falls back to a heap buffer (and then to pass-through) when there is no arena.
  BuildArena::Block writeBlock;
  uint8_t* writeBuf = nullptr;
  std::unique_ptr<uint8_t[]> heapWriteBuf;
  if (arena && arena->valid() && arena->capacity() - arena->used() >= EXTRACT_WRITE_BUFFER_BYTES) {
    writeBlock = arena->reserveBlock();
    writeBuf = static_cast<uint8_t*>(arena->alloc(EXTRACT_WRITE_BUFFER_BYTES));
  }
  if (!writeBuf) {
    heapWriteBuf = makeUniqueNoThrow<uint8_t[]>(EXTRACT_WRITE_BUFFER_BYTES);
    writeBuf = heapWriteBuf.get();
  }

  bool ok;
  {
    BufferedExtractSink sink(destFile, writeBuf, EXTRACT_WRITE_BUFFER_BYTES);
    ok = arena ? readItemContentsToStreamWithArena(itemHref, sink, arena, nullptr)
               : readItemContentsToStream(itemHref, sink, 1024);
    // Drain before the file is flushed/closed, and let a failed final write fail the extract:
    // a short file would otherwise pass as a complete one and be decoded as garbage.
    if (!sink.drain()) ok = false;
  }
  if (writeBlock.valid()) arena->release(writeBlock);

  destFile.flush();
  destFile.close();
  if (!ok) {
    Storage.remove(partPath.c_str());
    return false;
  }
  // FAT cannot rename onto an existing name; a stale extract gives way to the fresh one.
  if (Storage.exists(destPath.c_str())) Storage.remove(destPath.c_str());
  if (!Storage.rename(partPath.c_str(), destPath.c_str())) {
    LOG_ERR("EBP", "Failed to move extract into place: %s", destPath.c_str());
    Storage.remove(partPath.c_str());
    return false;
  }
  return true;
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath, BuildArena* arena) const {
  if (itemHref.empty() || destPath.empty()) return false;
  if (arena) {
    if (extractItemToFileOnce(itemHref, destPath, arena)) return true;
    // EntryReader does NOT fall back to the heap once it has been given an arena — a short
    // arena just makes open() fail. Retry on the heap so passing one can only ever add a way
    // to succeed. extractItemToFileOnce() removed the partial file, so this starts clean.
    LOG_DBG("EBP", "Arena extract failed for %s, retrying on the heap", itemHref.c_str());
  }
  return extractItemToFileOnce(itemHref, destPath, nullptr);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  ZipFile zip(filepath);
  primeZip(zip);
  const bool ok = zip.getInflatedFileSize(path.c_str(), size);
  adoptZipDetails(zip);
  return ok;
}

bool Epub::getStoredItemRange(const std::string& itemHref, uint32_t* offset, uint32_t* size) const {
  if (!offset || !size) return false;
  const std::string path = FsHelpers::normalisePath(itemHref);
  ZipFile zip(filepath);
  primeZip(zip);
  const bool ok = zip.getStoredEntryRange(path.c_str(), offset, size);
  adoptZipDetails(zip);
  return ok;
}

void Epub::ensureSpineStats() const {
  if (spineStatsResolved_) return;
  spineStatsResolved_ = true;  // one attempt per book instance; on failure fall back to scans

  const int spineCount = getSpineItemsCount();
  if (spineCount <= 0) return;

  // Heap gate: the deque needs ~sizeof(FileStatSlim) per spine plus a transient targets deque
  // of the same order during the walk. Require comfortable headroom so a genuinely low heap
  // falls back to per-spine scans (correct, slower) instead of aborting under -fno-exceptions.
  const size_t perSpine = sizeof(ZipFile::FileStatSlim) + sizeof(ZipFile::SizeTarget);
  const size_t needed = static_cast<size_t>(spineCount) * perSpine + 32u * 1024u;
  if (esp_get_free_heap_size() < needed) {
    LOG_INF("EBP", "spine-stat cache skipped (need %lu, free %lu) — falling back to per-spine scans",
            static_cast<unsigned long>(needed), static_cast<unsigned long>(esp_get_free_heap_size()));
    return;
  }

  ZipFile zip(filepath);
  primeZip(zip);

  // Build sorted (hash,len)->spineIndex targets, one central-directory walk resolves all stats.
  std::deque<ZipFile::SizeTarget> targets;
  targets.resize(spineCount);
  std::string pathScratch;
  for (int i = 0; i < spineCount; i++) {
    // Hrefs are already URI-decoded in the metadata cache; normalise to match the raw
    // central-directory names fillFileStats hashes (same keying fillUncompressedSizes uses).
    FsHelpers::normalisePath(getSpineItem(i).href, pathScratch);
    ZipFile::SizeTarget t;
    t.hash = HashUtils::fnvHash64(pathScratch.c_str(), pathScratch.size());
    t.len = static_cast<uint16_t>(pathScratch.size());
    t.index = static_cast<uint16_t>(i);
    targets[i] = t;
  }
  std::sort(targets.begin(), targets.end(), [](const ZipFile::SizeTarget& a, const ZipFile::SizeTarget& b) {
    return a.hash < b.hash || (a.hash == b.hash && a.len < b.len);
  });

  spineStats_.assign(spineCount, ZipFile::FileStatSlim{});
  const int matched = zip.fillFileStats(targets, spineStats_);
  adoptZipDetails(zip);
  targets.clear();
  targets.shrink_to_fit();

  if (matched != spineCount) {
    // Some spines didn't match the batch walk (hash mismatch / odd names). Keep the cache — the
    // matched entries still short-circuit; unmatched ones (localHeaderOffset==0) fall back per-call.
    LOG_INF("EBP", "spine-stat cache matched %d/%d — unmatched spines fall back to per-spine scans", matched,
            spineCount);
  }
  spineStatsUsable_ = matched > 0;
}

bool Epub::getSpineItemStat(const int spineIndex, ZipFile::FileStatSlim* out) const {
  if (!out || spineIndex < 0) return false;
  ensureSpineStats();

  if (spineStatsUsable_ && spineIndex < static_cast<int>(spineStats_.size())) {
    const ZipFile::FileStatSlim& s = spineStats_[spineIndex];
    // A matched entry has a non-zero local-header offset. Offset 0 is the archive's FIRST local
    // header — in an EPUB that is always the uncompressed `mimetype` entry (spec-mandated first,
    // STORED), never a spine XHTML, so 0 unambiguously means "unmatched in the batch walk" here.
    // A false 0 would only cost this one spine a fallback scan (still correct), not a wrong result.
    if (s.localHeaderOffset != 0) {
      *out = s;
      return true;
    }
  }

  // Fallback: single linear scan for this spine (cache disabled or this spine unmatched).
  ZipFile zip(filepath);
  primeZip(zip);
  const std::string entryPath = FsHelpers::normalisePath(getSpineItem(spineIndex).href);
  const bool ok = zip.loadFileStatSlim(entryPath.c_str(), out);
  adoptZipDetails(zip);
  return ok;
}

bool Epub::getSpineItemInflatedSize(const int spineIndex, size_t* size) const {
  if (bookMetadataCache && bookMetadataCache->isLoaded() && spineIndex >= 0 &&
      spineIndex < bookMetadataCache->getSpineCount()) {
    const size_t cumSize = getSpineItem(spineIndex).cumulativeSize;
    if (cumSize > 0) {
      const size_t prevCumSize = (spineIndex > 0) ? getSpineItem(spineIndex - 1).cumulativeSize : 0;
      *size = cumSize - prevCumSize;
      return true;
    }
  }
  return getItemSize(getSpineItem(spineIndex).href, size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const { return getSpineItem(spineIndex).cumulativeSize; }

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (syntheticTocFallbackEnabled && !hasReliableToc()) {
    const int spineCount = bookMetadataCache->getSpineCount();
    if (tocIndex < 0 || tocIndex >= spineCount) {
      LOG_DBG("EBP", "getTocItem synthetic index:%d is out of range", tocIndex);
      return {};
    }

    const auto spine = bookMetadataCache->getSpineEntry(tocIndex);
    return BookMetadataCache::TocEntry(tr(STR_SECTION_PREFIX) + std::to_string(tocIndex + 1), spine.href, "", 1,
                                       static_cast<int16_t>(tocIndex));
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  if (syntheticTocFallbackEnabled && !hasReliableToc()) {
    return bookMetadataCache->getSpineCount();
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (syntheticTocFallbackEnabled && !hasReliableToc()) {
    if (tocIndex < 0 || tocIndex >= bookMetadataCache->getSpineCount()) {
      LOG_ERR("EBP", "getSpineIndexForTocIndex synthetic tocIndex %d out of range", tocIndex);
      return 0;
    }
    return tocIndex;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

bool Epub::hasReliableToc() const {
  if (tocReliability != TocReliability::Unknown) {
    return tocReliability == TocReliability::Reliable;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    tocReliability = TocReliability::Unreliable;
    return false;
  }

  // Reliability is computed once at indexing time and persisted in book.bin's header.
  // This avoids the O(tocCount) seek-heavy scan that previously fired on first page load
  // for every book — a large web-novel TOC (~3000 entries) added several seconds of latency.
  const bool reliable = bookMetadataCache->isTocReliable();
  tocReliability = reliable ? TocReliability::Reliable : TocReliability::Unreliable;
  return reliable;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getTocIndexForSpineIndex called but cache not loaded");
    return -1;
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getTocIndexForSpineIndex: spineIndex %d out of range", spineIndex);
    return -1;
  }

  if (syntheticTocFallbackEnabled && !hasReliableToc()) {
    return spineIndex;
  }

  return bookMetadataCache->getSpineEntry(spineIndex).tocIndex;
}

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  const int spineCount = getSpineItemsCount();
  for (int i = 0; i < spineCount; i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

void Epub::streamPrintedPageEntries(
    const std::string& path,
    const std::function<bool(const std::string&, const std::string&, const std::string&)>& visit) {
  if (!Storage.exists(path.c_str())) {
    return;
  }
  FsFile f;
  if (!Storage.openFileForRead("EBP", path, f)) {
    return;
  }
  uint16_t count = 0;
  serialization::readPod(f, count);
  // Reused across iterations — labels/hrefs are short, so this never grows into the ~200 KB block
  // the full list would reserve. count is never used to pre-size anything: a corrupt/oversized
  // count simply runs the loop until readString hits EOF or a malformed field and we stop.
  std::string href, anchor, label;
  for (uint16_t i = 0; i < count; i++) {
    if (!serialization::readString(f, href) || !serialization::readString(f, anchor) ||
        !serialization::readString(f, label)) {
      break;  // malformed / oversized field: stop rather than risk desync
    }
    if (!visit(href, anchor, label)) {
      break;
    }
  }
  f.close();
}

// Mirror of parsePrintedPageLabel (EpubReaderActivity): non-empty, all digits, <= 999999.
static std::optional<int> parseNumericPageLabel(const std::string& label) {
  if (label.empty()) return std::nullopt;
  int value = 0;
  for (char c : label) {
    if (c < '0' || c > '9') return std::nullopt;
    value = value * 10 + (c - '0');
    if (value > 999999) return std::nullopt;
  }
  return value;
}

bool Epub::hasNumericPrintedPages() const {
  bool found = false;
  streamPrintedPageEntries(getCachePath() + "/pagelist.bin",
                           [&](const std::string&, const std::string&, const std::string& label) {
                             if (parseNumericPageLabel(label)) {
                               found = true;
                               return false;  // short-circuit
                             }
                             return true;
                           });
  return found;
}

bool Epub::getPrintedPageLabelRange(int& minLabel, int& maxLabel) const {
  int lo = std::numeric_limits<int>::max();
  int hi = std::numeric_limits<int>::min();
  streamPrintedPageEntries(getCachePath() + "/pagelist.bin",
                           [&](const std::string&, const std::string&, const std::string& label) {
                             if (const auto n = parseNumericPageLabel(label)) {
                               if (*n < lo) lo = *n;
                               if (*n > hi) hi = *n;
                             }
                             return true;
                           });
  if (hi < lo) return false;  // no numeric labels
  minLabel = lo;
  maxLabel = hi;
  return true;
}

std::optional<Epub::PrintedPageEntry> Epub::findPrintedPageByLabel(int target) const {
  std::optional<PrintedPageEntry> match;
  streamPrintedPageEntries(getCachePath() + "/pagelist.bin",
                           [&](const std::string& href, const std::string& anchor, const std::string& label) {
                             if (const auto n = parseNumericPageLabel(label); n && *n == target) {
                               match = PrintedPageEntry{href, anchor, label};
                               return false;  // first match wins (file order)
                             }
                             return true;
                           });
  return match;
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // The first spine item that matches exactly or by filename alone: in-book links are kept as
  // written, relative to their own document.
  for (int i = 0; i < getSpineItemsCount(); i++) {
    if (spine_href::match(target, getSpineItem(i).href) != SpineHrefMatch::None) return i;
  }
  return -1;
}
