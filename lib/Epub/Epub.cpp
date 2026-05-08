#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <PngToBmpConverter.h>
#include <ZipFile.h>

#include <cctype>
#include <cstring>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

namespace {

enum class CoverImageFormat { Unknown, Jpeg, Png };

CoverImageFormat detectCoverImageFormat(FsFile& imageFile) {
  if (!imageFile || !imageFile.seek(0)) {
    return CoverImageFormat::Unknown;
  }

  uint8_t header[8] = {};
  const int readBytes = imageFile.read(header, sizeof(header));
  imageFile.seek(0);

  if (readBytes >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
    return CoverImageFormat::Jpeg;
  }

  constexpr uint8_t PNG_SIGNATURE[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (readBytes >= 8 && memcmp(header, PNG_SIGNATURE, sizeof(PNG_SIGNATURE)) == 0) {
    return CoverImageFormat::Png;
  }

  return CoverImageFormat::Unknown;
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

  const unsigned long streamStart = millis();
  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024)) {
    LOG_ERR("EBP", "Could not read content.opf");
    return false;
  }
  const unsigned long streamMs = millis() - streamStart;

  // Grab data from opfParser into epub
  bookMetadata.title = opfParser.title;
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;
  bookMetadata.series = opfParser.series;
  bookMetadata.seriesIndex = opfParser.seriesIndex;
  bookMetadata.description = opfParser.description;

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
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Check if it's an image file
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref) || FsHelpers::hasGifExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(coverPageBase + imageRef);
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  auto isSupportedCoverType = [](const std::string& path) {
    return FsHelpers::hasJpgExtension(path) || FsHelpers::hasPngExtension(path);
  };

  auto hasReadableSupportedCover = [&](const std::string& path) {
    if (path.empty() || !isSupportedCoverType(path)) return false;
    size_t coverSize = 0;
    return getItemSize(path, &coverSize);
  };

  if (!hasReadableSupportedCover(bookMetadata.coverItemHref)) {
    if (!bookMetadata.coverItemHref.empty()) {
      LOG_DBG("EBP", "Cover href unresolved/unsupported, trying common cover candidates: %s",
              bookMetadata.coverItemHref.c_str());
    }

    std::vector<std::string> baseDirs;
    auto addBaseDir = [&](const std::string& dir) {
      if (dir.empty()) {
        for (const auto& existing : baseDirs) {
          if (existing.empty()) return;
        }
        baseDirs.emplace_back();
        return;
      }

      const std::string normalized = FsHelpers::normalisePath(dir);
      const std::string withSlash = normalized.empty() ? std::string() : normalized + "/";
      for (const auto& existing : baseDirs) {
        if (existing == withSlash) return;
      }
      baseDirs.push_back(withSlash);
    };

    // 1) OPF directory first (most likely)
    // 2) Parent dir of OPF directory
    // 3) Common EPUB roots
    // 4) Archive root
    addBaseDir(contentBasePath);
    if (!contentBasePath.empty()) {
      const auto trimmed =
          contentBasePath.back() == '/' ? contentBasePath.substr(0, contentBasePath.size() - 1) : contentBasePath;
      const auto lastSlash = trimmed.rfind('/');
      if (lastSlash != std::string::npos) {
        addBaseDir(trimmed.substr(0, lastSlash + 1));
      }
    }
    addBaseDir("OEBPS/");
    addBaseDir("OPS/");
    addBaseDir("EPUB/");
    addBaseDir("");

    static constexpr const char* kCoverSubdirs[] = {
        "", "images/", "Images/", "image/", "img/", "graphics/",
    };

    static constexpr const char* kCoverBaseNames[] = {
        "cover", "frontcover", "titlepage", "title", "cover-image", "coverimage",
    };

    static constexpr const char* kCoverExtensions[] = {
        "jpg",
        "jpeg",
        "png",
    };

    auto toUpper = [](std::string value) {
      for (char& ch : value) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
      }
      return value;
    };

    const unsigned long coverBatchStart = millis();
    ZipFile zip(filepath);
    const bool zipIndexLoaded = zip.loadAllFileStatSlims();
    LOG_DBG("EBP", "Common cover fallback indexed ZIP in %lu ms (ok=%d)", millis() - coverBatchStart,
            zipIndexLoaded ? 1 : 0);

    if (zipIndexLoaded) {
      int checkedCandidates = 0;
      const auto tryCandidate = [&](const std::string& candidate) {
        checkedCandidates++;
        size_t coverSize = 0;
        if (zip.getInflatedFileSize(candidate.c_str(), &coverSize) && coverSize > 0) {
          bookMetadata.coverItemHref = candidate;
          LOG_DBG("EBP", "Found cover image via common candidate fallback after %d checks in %lu ms: %s",
                  checkedCandidates, millis() - coverBatchStart, bookMetadata.coverItemHref.c_str());
          return true;
        }
        return false;
      };

      bool foundCoverCandidate = false;
      for (const auto& baseDir : baseDirs) {
        for (const char* subDir : kCoverSubdirs) {
          for (const char* baseName : kCoverBaseNames) {
            const std::string lowerBase = baseName;
            const std::string upperBase = toUpper(lowerBase);
            for (const std::string* baseVariant : {&lowerBase, &upperBase}) {
              for (const char* ext : kCoverExtensions) {
                const std::string lowerExt = ext;
                const std::string upperExt = toUpper(lowerExt);
                for (const std::string* extVariant : {&lowerExt, &upperExt}) {
                  const std::string candidate =
                      FsHelpers::normalisePath(baseDir + subDir + *baseVariant + "." + *extVariant);
                  if (tryCandidate(candidate)) {
                    foundCoverCandidate = true;
                    break;
                  }
                }
                if (foundCoverCandidate) break;
              }
              if (foundCoverCandidate) break;
            }
            if (foundCoverCandidate) break;
          }
          if (foundCoverCandidate) break;
        }
        if (foundCoverCandidate) break;
      }

      if (!foundCoverCandidate) {
        LOG_DBG("EBP", "Common cover fallback checked %d cached candidates in %lu ms with no match", checkedCandidates,
                millis() - coverBatchStart);
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "parseContentOpf total=%lu ms", millis() - opfParseStart);
  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  const auto tmpNcxPath = getCachePath() + "/toc.ncx";
  FsFile tempNcxFile;
  if (!Storage.openFileForWrite("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  readItemContentsToStream(tocNcxItem, tempNcxFile, 1024);
  tempNcxFile.close();
  if (!Storage.openFileForRead("EBP", tmpNcxPath, tempNcxFile)) {
    return false;
  }
  const auto ncxSize = tempNcxFile.size();

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  const auto ncxBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!ncxBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc ncx parser");
    tempNcxFile.close();
    return false;
  }

  while (tempNcxFile.available()) {
    const auto readSize = tempNcxFile.read(ncxBuffer, 1024);
    if (readSize == 0) break;
    const auto processedSize = ncxParser.write(ncxBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc ncx data");
      free(ncxBuffer);
      tempNcxFile.close();
      return false;
    }
  }

  free(ncxBuffer);
  tempNcxFile.close();
  Storage.remove(tmpNcxPath.c_str());

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

  const auto tmpNavPath = getCachePath() + "/toc.nav";
  FsFile tempNavFile;
  if (!Storage.openFileForWrite("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  readItemContentsToStream(tocNavItem, tempNavFile, 1024);
  tempNavFile.close();
  if (!Storage.openFileForRead("EBP", tmpNavPath, tempNavFile)) {
    return false;
  }
  const auto navSize = tempNavFile.size();

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  const auto navBuffer = static_cast<uint8_t*>(malloc(1024));
  if (!navBuffer) {
    LOG_ERR("EBP", "Could not allocate memory for toc nav parser");
    return false;
  }

  while (tempNavFile.available()) {
    const auto readSize = tempNavFile.read(navBuffer, 1024);
    const auto processedSize = navParser.write(navBuffer, readSize);

    if (processedSize != readSize) {
      LOG_ERR("EBP", "Could not process all toc nav data");
      free(navBuffer);
      tempNavFile.close();
      return false;
    }
  }

  free(navBuffer);
  tempNavFile.close();
  Storage.remove(tmpNavPath.c_str());

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 64 * 1024;  // 64KB

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
  for (const auto& cssPath : cssFiles) {
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
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
    cssParser->loadFromStream(tempCssFile);
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
  }

  // Save to cache for next time
  if (!cssParser->saveToCache()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }
  cssParser->clear();

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files", cssParser->ruleCount(), cssFiles.size());
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());
  tocReliabilityState = -1;

  // Initialize spine/TOC cache
  bookMetadataCache.reset(new BookMetadataCache(cachePath));
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser.reset(new CssParser(cachePath));

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        if (!parseContentOpf(bookMetadataCache->coreMetadata, OpfCacheMode::Disabled)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        }
        parseCssFiles();
        // Invalidate section caches so they are rebuilt with the new CSS
        Storage.removeDir((cachePath + "/sections").c_str());
      }
    }
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

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

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
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
    // Parse CSS files after cache reload
    parseCssFiles();
    Storage.removeDir((cachePath + "/sections").c_str());
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }

  Storage.mkdir(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

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

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_ERR("EBP", "No known cover image");
    return false;
  }

  const auto coverTempPath = getCachePath() + "/.cover.img";

  FsFile coverImage;
  if (!Storage.openFileForWrite("EBP", coverTempPath, coverImage)) {
    return false;
  }

  if (!readItemContentsToStream(coverImageHref, coverImage, 1024)) {
    LOG_ERR("EBP", "Failed to read cover image from EPUB: %s", coverImageHref.c_str());
    coverImage.close();
    Storage.remove(coverTempPath.c_str());
    return false;
  }

  coverImage.close();

  if (!Storage.openFileForRead("EBP", coverTempPath, coverImage)) {
    return false;
  }

  if (coverImage.size() == 0) {
    LOG_ERR("EBP", "Cover image extracted as empty file: %s", coverImageHref.c_str());
    coverImage.close();
    Storage.remove(coverTempPath.c_str());
    return false;
  }

  const auto detectedFormat = detectCoverImageFormat(coverImage);
  if (detectedFormat == CoverImageFormat::Jpeg) {
    LOG_DBG("EBP", "Generating BMP from JPEG cover image (%s mode)", cropped ? "cropped" : "fit");
  } else if (detectedFormat == CoverImageFormat::Png) {
    LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode)", cropped ? "cropped" : "fit");
  } else {
    LOG_ERR("EBP", "Cover image has unsupported format: %s", coverImageHref.c_str());
    coverImage.close();
    Storage.remove(coverTempPath.c_str());
    return false;
  }

  FsFile coverBmp;
  if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
    coverImage.close();
    return false;
  }

  bool success = false;
  if (detectedFormat == CoverImageFormat::Jpeg) {
    success = JpegToBmpConverter::jpegFileToBmpStream(coverImage, coverBmp, cropped);
  } else {
    success = PngToBmpConverter::pngFileToBmpStream(coverImage, coverBmp, cropped);
  }

  coverImage.close();
  coverBmp.close();
  Storage.remove(coverTempPath.c_str());

  if (!success) {
    LOG_ERR("EBP", "Failed to generate BMP from cover image");
    Storage.remove(getCoverBmpPath(cropped).c_str());
  }

  LOG_DBG("EBP", "Generated BMP from cover image, success: %s", success ? "yes" : "no");
  return success;
}

std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const { return cachePath + "/thumb_" + std::to_string(height) + ".bmp"; }

bool Epub::generateThumbBmp(int height) const {
  // Already generated, return true
  if (Storage.exists(getThumbBmpPath(height).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  const auto coverImageHref = bookMetadataCache->coreMetadata.coverItemHref;
  if (coverImageHref.empty()) {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  } else {
    const auto coverTempPath = getCachePath() + "/.cover.img";

    FsFile coverImage;
    if (!Storage.openFileForWrite("EBP", coverTempPath, coverImage)) {
      return false;
    }

    if (!readItemContentsToStream(coverImageHref, coverImage, 1024)) {
      LOG_ERR("EBP", "Failed to read cover image for thumbnail: %s", coverImageHref.c_str());
      coverImage.close();
      Storage.remove(coverTempPath.c_str());
      return false;
    }

    coverImage.close();

    if (!Storage.openFileForRead("EBP", coverTempPath, coverImage)) {
      return false;
    }

    if (coverImage.size() == 0) {
      LOG_ERR("EBP", "Cover image for thumbnail extracted as empty file: %s", coverImageHref.c_str());
      coverImage.close();
      Storage.remove(coverTempPath.c_str());
      return false;
    }

    const auto detectedFormat = detectCoverImageFormat(coverImage);
    if (detectedFormat == CoverImageFormat::Unknown) {
      LOG_ERR("EBP", "Cover image is not a supported format, skipping thumbnail: %s", coverImageHref.c_str());
      coverImage.close();
      Storage.remove(coverTempPath.c_str());
      return false;
    }

    FsFile thumbBmp;
    if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
      coverImage.close();
      return false;
    }

    // Use smaller target size for Continue Reading card (half of screen: 240x400)
    // Generate 1-bit BMP for fast home screen rendering (no gray passes needed)
    int THUMB_TARGET_WIDTH = height * 0.6;
    int THUMB_TARGET_HEIGHT = height;

    bool success = false;
    if (detectedFormat == CoverImageFormat::Jpeg) {
      LOG_DBG("EBP", "Generating thumb BMP from JPEG cover image");
      success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, THUMB_TARGET_WIDTH,
                                                                    THUMB_TARGET_HEIGHT);
    } else {
      LOG_DBG("EBP", "Generating thumb BMP from PNG cover image");
      success = PngToBmpConverter::pngFileTo1BitBmpStreamWithSize(coverImage, thumbBmp, THUMB_TARGET_WIDTH,
                                                                  THUMB_TARGET_HEIGHT);
    }

    coverImage.close();
    thumbBmp.close();
    Storage.remove(coverTempPath.c_str());

    if (!success) {
      LOG_ERR("EBP", "Failed to generate thumb BMP from cover image");
      Storage.remove(getThumbBmpPath(height).c_str());
    }
    LOG_DBG("EBP", "Generated thumb BMP from cover image, success: %s", success ? "yes" : "no");
    return success;
  }

  // Write an empty bmp file to avoid generation attempts in the future
  FsFile thumbBmp;
  Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp);
  thumbBmp.close();
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize);
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
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
  if (tocReliabilityState != -1) {
    return tocReliabilityState == 1;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    tocReliabilityState = 0;
    return false;
  }

  // Reliability is computed once at indexing time and persisted in book.bin's header.
  // This avoids the O(tocCount) seek-heavy scan that previously fired on first page load
  // for every book — a large web-novel TOC (~3000 entries) added several seconds of latency.
  const bool reliable = bookMetadataCache->isTocReliable();
  tocReliabilityState = reliable ? 1 : 0;
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
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
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

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Extract filename (remove #anchor)
  std::string target = href;
  size_t hashPos = target.find('#');
  if (hashPos != std::string::npos) target = target.substr(0, hashPos);

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
