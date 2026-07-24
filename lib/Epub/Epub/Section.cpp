#include "Section.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>

#include <algorithm>
#include <array>
#include <cstring>

#include "BoundedFileReader.h"
#include "Epub/css/CssParser.h"
#include "Page.h"
#include "SectionCacheLayout.h"
#include "SectionCacheValidator.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
// v28: text decoration bits now include line-through in serialized wordStyles.
// v29: TextBlock word data stored as one flat arena (offset table + NUL-terminated
// text blob) instead of length-prefixed strings and per-field arrays.
// v30: Arabic shaping changed both drawing and measurement (getTextAdvanceX now
//      measures the shaped visual text); cached word positions from v29 no longer
//      match what drawText renders.
// v31: EPUB render mode and forced paragraph indentation are layout inputs.
constexpr uint8_t SECTION_FILE_VERSION = 31;
// Written into the version field while a build is in progress; patched to
// SECTION_FILE_VERSION only when the build is finalized. An abandoned /
// crash-interrupted .bin therefore carries version 0, which loadSectionFile rejects
// as unknown and clears -- so an incomplete file is never mistaken for a valid one.
constexpr uint8_t SECTION_FILE_INCOMPLETE_VERSION = 0;
// Written when a build is suspended partway (reader exited or device slept mid-build).
// The file carries valid pages 0..pageCount-1, all LUTs, and a trailer with the parse
// watermark (bytesConsumed, totalBytes) appended after the li LUT. loadSectionFile
// accepts it so a resume shows those pages instantly; the reader extends it by
// rebuilding in the background. Uses the same header layout as SECTION_FILE_VERSION,
// so finalized files are untouched by this feature; older firmware treats the sentinel
// as an unknown version and rebuilds, which is a safe downgrade.
// MUST change in lockstep with SECTION_FILE_VERSION: the sentinel IS the partial's
// format version, so a stale-format partial otherwise passes the header check and
// only fails (noisily, via the block-decode error path) when a page is loaded.
// Derived so the pairing can't be forgotten: 0xFE for v28, 0xFD for v29, ...
constexpr uint8_t SECTION_FILE_PARTIAL_VERSION = 0xFE - (SECTION_FILE_VERSION - 28);
constexpr uint32_t HEADER_SIZE = sizeof(uint8_t) + sizeof(int) + sizeof(float) + sizeof(bool) + sizeof(uint8_t) +
                                 sizeof(uint16_t) + sizeof(uint16_t) + sizeof(uint16_t) + sizeof(bool) + sizeof(bool) +
                                 sizeof(uint8_t) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t);
static_assert(sizeof(int) == sizeof(int32_t) && sizeof(float) == sizeof(uint32_t) && sizeof(bool) == sizeof(uint8_t),
              "Section cache validation requires the serialized scalar layout");

template <typename T>
bool readPodExact(HalFile& file, T& value) {
  value = {};
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

bool readBoolExact(HalFile& file, bool& value) {
  uint8_t serialized = 0;
  if (!readPodExact(file, serialized) || serialized > 1) {
    value = false;
    return false;
  }
  value = serialized != 0;
  return true;
}

class HalFileValidationInput final : public SectionCacheValidation::Input {
 public:
  explicit HalFileValidationInput(HalFile& file) : file_(file), size_(file.fileSize64()) {}

  uint64_t size() const override { return size_; }

  bool readAt(const uint64_t offset, void* destination, const size_t length) override {
    if ((!destination && length != 0) || offset > size_ || length > size_ - offset) return false;
    if (position_ != offset && !file_.seek64(offset)) return false;
    if (length != 0 && file_.read(destination, length) != static_cast<int>(length)) {
      position_ = UINT64_MAX;
      return false;
    }
    position_ = offset + length;
    return true;
  }

 private:
  HalFile& file_;
  uint64_t size_ = 0;
  uint64_t position_ = UINT64_MAX;
};

bool validateSectionCache(HalFile& file, SectionCacheValidation::Layout& layout) {
  HalFileValidationInput input(file);
  return SectionCacheValidation::validate(input, HEADER_SIZE, SECTION_FILE_VERSION, SECTION_FILE_PARTIAL_VERSION,
                                          layout);
}

bool readStringEquals(BoundedFileReader& reader, const uint32_t length, const std::string& expected, bool& matches) {
  matches = length == expected.size();
  std::array<uint8_t, 64> buffer{};
  uint64_t consumed = 0;
  while (consumed < length) {
    const size_t chunk = static_cast<size_t>(std::min<uint64_t>(buffer.size(), length - consumed));
    if (!reader.readBytes(buffer.data(), chunk)) return false;
    if (matches && memcmp(buffer.data(), expected.data() + consumed, chunk) != 0) matches = false;
    consumed += chunk;
  }
  return true;
}
}  // namespace

// Out-of-line so the unique_ptr<ChapterHtmlSlimParser> in BuildContext can be
// constructed/destroyed where the parser's full definition is visible.
Section::Section(const std::shared_ptr<Epub>& epub, const int spineIndex, GfxRenderer& renderer)
    : epub(epub),
      spineIndex(spineIndex),
      renderer(renderer),
      filePath(epub->getCachePath() + "/sections/" + std::to_string(spineIndex) + ".bin") {}

// Suspend any in-progress build so every section.reset() / navigation / sleep path
// persists the pages already laid out as a partial .bin instead of discarding them
// (no-op once a build has completed or never started).
Section::~Section() { suspendBuild(); }

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", builtPageCount_);
    return 0;
  }
  if (!page || builtPageCount_ == UINT16_MAX) {
    LOG_ERR("SCT", "Cannot serialize another page");
    return 0;
  }

  const uint32_t position = file.position();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", builtPageCount_);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed", builtPageCount_);

  builtPageCount_++;
  // pageCount is the pages available to read: a rebuild over a partial only raises it
  // once it has laid out more pages than the partial already covers.
  if (builtPageCount_ > pageCount) {
    pageCount = builtPageCount_;
  }
  return position;
}

void Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                     const uint16_t viewportHeight, const bool hyphenationEnabled,
                                     const bool embeddedStyle, const uint8_t imageRendering,
                                     const bool focusReadingEnabled, const EpubRenderMode renderMode,
                                     const bool forceParagraphIndents) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_FILE_VERSION) + sizeof(fontId) + sizeof(lineCompression) +
                                   sizeof(extraParagraphSpacing) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(focusReadingEnabled) +
                                   sizeof(renderMode) + sizeof(forceParagraphIndents) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  // Written as the incomplete sentinel; finalizeBuild() patches it to
  // SECTION_FILE_VERSION as the last step, committing the file.
  serialization::writePod(file, SECTION_FILE_INCOMPLETE_VERSION);
  serialization::writePod(file, fontId);
  serialization::writePod(file, lineCompression);
  serialization::writePod(file, extraParagraphSpacing);
  serialization::writePod(file, paragraphAlignment);
  serialization::writePod(file, viewportWidth);
  serialization::writePod(file, viewportHeight);
  serialization::writePod(file, hyphenationEnabled);
  serialization::writePod(file, embeddedStyle);
  serialization::writePod(file, imageRendering);
  serialization::writePod(file, focusReadingEnabled);
  serialization::writePod(file, static_cast<uint8_t>(renderMode));
  serialization::writePod(file, forceParagraphIndents);
  serialization::writePod(file, pageCount);  // Placeholder for page count (will be initially 0, patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for anchor map offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for paragraph LUT offset (patched later)
  serialization::writePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                              const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                              const uint8_t imageRendering, const bool focusReadingEnabled,
                              const EpubRenderMode renderMode, const bool forceParagraphIndents) {
  cacheLayout_ = {};
  cacheLayoutValid_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  pageCount = 0;
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  if (!SectionCacheLayout::hasCompleteHeader(file.fileSize64(), HEADER_SIZE)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: truncated header");
    clearCache();
    return false;
  }

  // Match parameters
  bool filePartial = false;
  uint8_t fileVersion = 0;
  {
    if (!readPodExact(file, fileVersion)) {
      file.close();
      clearCache();
      return false;
    }
    if (fileVersion != SECTION_FILE_VERSION && fileVersion != SECTION_FILE_PARTIAL_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", fileVersion);
      clearCache();
      return false;
    }
    filePartial = (fileVersion == SECTION_FILE_PARTIAL_VERSION);

    int fileFontId{};
    uint16_t fileViewportWidth{};
    uint16_t fileViewportHeight{};
    float fileLineCompression{};
    bool fileExtraParagraphSpacing{};
    uint8_t fileParagraphAlignment{};
    bool fileHyphenationEnabled{};
    bool fileEmbeddedStyle{};
    uint8_t fileImageRendering{};
    bool fileFocusReadingEnabled{};
    uint8_t fileRenderMode{};
    bool fileForceParagraphIndents{};
    if (!readPodExact(file, fileFontId) || !readPodExact(file, fileLineCompression) ||
        !readBoolExact(file, fileExtraParagraphSpacing) || !readPodExact(file, fileParagraphAlignment) ||
        !readPodExact(file, fileViewportWidth) || !readPodExact(file, fileViewportHeight) ||
        !readBoolExact(file, fileHyphenationEnabled) || !readBoolExact(file, fileEmbeddedStyle) ||
        !readPodExact(file, fileImageRendering) || !readBoolExact(file, fileFocusReadingEnabled) ||
        !readPodExact(file, fileRenderMode) || !readBoolExact(file, fileForceParagraphIndents) ||
        !readPodExact(file, pageCount)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: unreadable header");
      clearCache();
      pageCount = 0;
      return false;
    }

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || paragraphAlignment != fileParagraphAlignment ||
        viewportWidth != fileViewportWidth || viewportHeight != fileViewportHeight ||
        hyphenationEnabled != fileHyphenationEnabled || embeddedStyle != fileEmbeddedStyle ||
        imageRendering != fileImageRendering || focusReadingEnabled != fileFocusReadingEnabled ||
        static_cast<uint8_t>(renderMode) != fileRenderMode || forceParagraphIndents != fileForceParagraphIndents) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  SectionCacheValidation::Layout validated;
  if (!validateSectionCache(file, validated) || validated.version != fileVersion || validated.pageCount != pageCount ||
      validated.partial != filePartial) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: malformed section structure");
    clearCache();
    pageCount = 0;
    return false;
  }

  cacheLayout_ = validated;
  cacheLayoutValid_ = true;
  if (filePartial) {
    partialBytesConsumed_ = validated.partialBytesConsumed;
    partialTotalBytes_ = validated.partialTotalBytes;
    partial_ = true;
    partialPageCount_ = pageCount;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  LOG_DBG("SCT", "Deserialization succeeded: %d pages%s", pageCount, filePartial ? " (partial)" : "");
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() {
  cacheLayout_ = {};
  cacheLayoutValid_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  partialBytesConsumed_ = 0;
  partialTotalBytes_ = 0;
  pageCount = 0;
  const std::string tmpBin = binTmpPath();
  if (Storage.exists(tmpBin.c_str())) {
    Storage.remove(tmpBin.c_str());
  }
  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                const uint16_t viewportHeight, const bool hyphenationEnabled, const bool embeddedStyle,
                                const uint8_t imageRendering, const bool focusReadingEnabled,
                                const EpubRenderMode renderMode, const bool forceParagraphIndents,
                                const std::function<void()>& popupFn) {
  // One-shot build: start, then lay out the whole section in a single pass.
  if (!startBuild(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth, viewportHeight,
                  hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled, renderMode,
                  forceParagraphIndents, popupFn)) {
    return false;
  }
  if (!buildSomeMore(0)) {  // 0 = build to completion
    return false;
  }
  return buildComplete_;
}

bool Section::startBuild(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                         const uint8_t paragraphAlignment, const uint16_t viewportWidth, const uint16_t viewportHeight,
                         const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                         const bool focusReadingEnabled, const EpubRenderMode renderMode,
                         const bool forceParagraphIndents, const std::function<void()>& popupFn) {
  if (build_) {
    LOG_ERR("SCT", "startBuild called while a build is already active");
    return false;
  }
  buildComplete_ = false;
  lastBuildStatus_ = EpubBuildStatus::Ok;
  builtPageCount_ = 0;
  if (!partial_) {
    cacheLayout_ = {};
    cacheLayoutValid_ = false;
  }
  // Pages from a loaded partial stay readable (from filePath) while this build writes
  // to the tmp .bin, so availability never drops below the partial's watermark.
  pageCount = partial_ ? partialPageCount_ : 0;

  // Remove a stale tmp .bin from a crash-interrupted build; this build recreates it.
  {
    const std::string staleTmp = binTmpPath();
    if (Storage.exists(staleTmp.c_str())) {
      Storage.remove(staleTmp.c_str());
    }
  }

  const auto localPath = epub->getSpineItem(spineIndex).href;
  const auto htmlDir = epub->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(spineIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(spineIndex) + ".html";

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = epub->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // even a window-only giant spine -- whose .bin never finalizes -- still caches its HTML, letting a
  // reopen skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      // Larger chunks mean far fewer SD writes inflating the HTML; a 1KB chunk turned a 584KB
      // single-spine novel into ~570 tiny writes (multi-second). 8KB keeps the transient buffers
      // small while cutting the write count 8x.
      streamed = epub->readItemContentsToStream(localPath, tmpHtml, 8192);
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      lastBuildStatus_ = EpubBuildStatus::IoError;
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes)", tmpHtmlPath.c_str(), fileSize);

    // Promote to the persistent HTML cache immediately -- the inflate is complete and the bytes are
    // valid regardless of whether the layout build finishes, so reopening (even a window-only spine
    // that never finalizes its .bin) skips re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }

  if (!Storage.openFileForWrite("SCT", binTmpPath(), file)) {
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    lastBuildStatus_ = EpubBuildStatus::IoError;
    return false;
  }
  // Header is written with the incomplete-version sentinel; finalizeBuild() commits it.
  writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, paragraphAlignment, viewportWidth,
                         viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering, focusReadingEnabled,
                         renderMode, forceParagraphIndents);

  auto ctx = makeUniqueNoThrow<BuildContext>();
  if (!ctx) {
    LOG_ERR("SCT", "OOM: BuildContext");
    lastBuildStatus_ = EpubBuildStatus::OutOfMemory;
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }
  // htmlCached == "htmlPath is the live cache" (reused, or just promoted). finalizeBuild/abandonBuild
  // then leave the cached HTML alone; only an un-promoted temp (rename failed) is theirs to clean up.
  ctx->reusedHtml = htmlCached;
  ctx->htmlPath = htmlPath;
  ctx->tmpHtmlPath = tmpHtmlPath;
  ctx->parsePath = htmlCached ? htmlPath : tmpHtmlPath;

  // Derive the content base directory and image cache path prefix for the parser
  const size_t lastSlash = localPath.find_last_of('/');
  ctx->contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  ctx->imageBasePath = epub->getCachePath() + "/img_" + std::to_string(spineIndex) + "_";

  if (embeddedStyle) {
    ctx->cssParser = epub->getCssParser();
    if (ctx->cssParser && !ctx->cssParser->loadFromCache()) {
      LOG_ERR("SCT", "Failed to load CSS from cache");
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = epub->getTocIndexForSpineIndex(spineIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < epub->getTocItemsCount(); i++) {
      auto entry = epub->getTocItem(i);
      if (entry.spineIndex != spineIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  // The parser stores the path/contentBase/imageBasePath by reference, so they must
  // live in the BuildContext (which outlives the parser). The page-complete callback
  // captures the BuildContext pointer to append to its in-RAM LUT; build_ owns the
  // context for the parser's whole lifetime.
  BuildContext* ctxPtr = ctx.get();
  ctx->parser = makeUniqueNoThrow<ChapterHtmlSlimParser>(
      epub, ctxPtr->parsePath, renderer, fontId, lineCompression, extraParagraphSpacing, paragraphAlignment,
      viewportWidth, viewportHeight, hyphenationEnabled, focusReadingEnabled,
      [this, ctxPtr](std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
        ctxPtr->lut.push_back({this->onPageComplete(std::move(page)), paragraphIndex, listItemIndex});
      },
      embeddedStyle, ctxPtr->contentBase, ctxPtr->imageBasePath, imageRendering, std::move(tocAnchors), popupFn,
      ctxPtr->cssParser, renderMode, forceParagraphIndents);
  if (!ctx->parser) {
    LOG_ERR("SCT", "OOM: ChapterHtmlSlimParser");
    lastBuildStatus_ = EpubBuildStatus::OutOfMemory;
    if (ctx->cssParser) ctx->cssParser->clear();
    file.close();
    Storage.remove(binTmpPath().c_str());
    if (!reusedHtml) Storage.remove(tmpHtmlPath.c_str());
    return false;
  }

  Hyphenator::setPreferredLanguage(epub->getLanguage());
  build_ = std::move(ctx);

  if (!build_->parser->beginParse()) {
    LOG_ERR("SCT", "Failed to begin parse");
    lastBuildStatus_ = build_->parser->lastFailure() == ChapterParseFailure::OutOfMemory
                           ? EpubBuildStatus::OutOfMemory
                           : (build_->parser->lastFailure() == ChapterParseFailure::IoError
                                  ? EpubBuildStatus::IoError
                                  : EpubBuildStatus::InvalidContent);
    abandonBuild();
    return false;
  }
  build_->totalBytes = build_->parser->parseTotalBytes();
  return true;
}

bool Section::buildSomeMore(const int maxPages) {
  if (!build_ || !build_->parser) {
    LOG_ERR("SCT", "buildSomeMore with no active build");
    return false;
  }
  // Pace on pages laid out by THIS build, not pageCount: during a rebuild over a partial,
  // pageCount stays pinned at the partial's watermark until the build passes it, which
  // would otherwise turn one "small" chunk into a blocking rebuild of the whole watermark.
  const int startCount = builtPageCount_;
  for (;;) {
    const auto status = build_->parser->parseStep();
    if (status == ChapterHtmlSlimParser::ParseStatus::Error) {
      LOG_ERR("SCT", "Parse error during incremental build");
      lastBuildStatus_ = build_->parser->lastFailure() == ChapterParseFailure::OutOfMemory
                             ? EpubBuildStatus::OutOfMemory
                             : (build_->parser->lastFailure() == ChapterParseFailure::IoError
                                    ? EpubBuildStatus::IoError
                                    : EpubBuildStatus::InvalidContent);
      abandonBuild();
      return false;
    }
    if (status == ChapterHtmlSlimParser::ParseStatus::Done) {
      return finalizeBuild();
    }
    // ParseStatus::More: yield once we've laid out the requested number of pages.
    if (maxPages > 0 && (builtPageCount_ - startCount) >= maxPages) {
      build_->bytesConsumed = build_->parser->parseBytesConsumed();
      return true;
    }
  }
}

bool Section::hasHtmlCache() const {
  const std::string htmlPath = epub->getCachePath() + "/html/" + std::to_string(spineIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::optional<uint16_t> Section::findAnchorDuringBuild(const std::string& anchor) const {
  if (!build_ || !build_->parser) return std::nullopt;
  for (const auto& [key, page] : build_->parser->getAnchors()) {
    if (key == anchor) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::findAnchor(const std::string& anchor) const {
  if (const auto page = findAnchorDuringBuild(anchor)) {
    return page;
  }
  // Fall back to the on-disk anchor map: a finalized section, or a partial whose map
  // covers everything up to its watermark (nullopt past it -- build further and retry).
  return getPageForAnchor(anchor);
}

uint16_t Section::estimatedTotalPages() const {
  // Extrapolation from a suspended session's watermark trailer. A static snapshot, so no EMA
  // damping is needed. Also the best guess while a rebuild is running but hasn't laid out
  // enough pages yet to extrapolate from its own progress.
  const auto partialEstimate = [this]() -> uint16_t {
    if (!partial_ || partialBytesConsumed_ == 0 || partialTotalBytes_ <= partialBytesConsumed_) {
      return pageCount;
    }
    const uint64_t est = static_cast<uint64_t>(partialPageCount_) * partialTotalBytes_ / partialBytesConsumed_;
    if (est <= pageCount) return pageCount;
    return est > 60000 ? 60000 : static_cast<uint16_t>(est);
  };

  if (!build_) {
    return partial_ ? partialEstimate() : pageCount;  // partial -> extrapolate, finalized -> exact
  }
  const uint32_t consumed = build_->bytesConsumed;
  const uint32_t total = build_->totalBytes;
  if (builtPageCount_ == 0 || consumed == 0 || total <= consumed) return partialEstimate();

  // Raw extrapolation: scale the pages built so far by the fraction of HTML still unparsed. This
  // re-derives from a growing, non-uniform sample, so it jitters up and down as the build crosses
  // dense vs sparse regions of the chapter.
  const uint64_t raw = static_cast<uint64_t>(builtPageCount_) * total / consumed;

  // Damp that jitter with an exponential moving average. Step it once per build advance (keyed on
  // bytesConsumed) rather than per status-bar redraw, so the smoothing rate doesn't depend on how
  // often we repaint. As the build nears the end, consumed -> total and raw -> the built count, so
  // the average settles onto the true count (and finalizeBuild then returns the exact pageCount).
  constexpr float ALPHA = 0.25f;  // weight of each new sample; lower = steadier but slower to settle
  if (build_->smoothedEstimate <= 0) {
    build_->smoothedEstimate = static_cast<float>(raw);  // seed on the first estimate
  } else if (consumed != build_->smoothedAtConsumed) {
    build_->smoothedEstimate += ALPHA * (static_cast<float>(raw) - build_->smoothedEstimate);
  }
  build_->smoothedAtConsumed = consumed;

  const uint64_t est = static_cast<uint64_t>(build_->smoothedEstimate + 0.5f);
  if (est <= pageCount) return pageCount;  // never fewer than the pages already available
  return est > 60000 ? 60000 : static_cast<uint16_t>(est);
}

// Write the LUTs and anchor map into the open tmp .bin, patch the header with the built
// page count and table offsets, stamp `version` as the commit point, then swap the tmp
// file over filePath. For SECTION_FILE_PARTIAL_VERSION a watermark trailer
// (bytesConsumed, totalBytes) is appended after the li LUT so a later open can estimate
// the total page count. The parser must still be alive (anchors are read from it).
// On failure the tmp is removed and any pre-existing file at filePath is left intact.
bool Section::commitBuildFile(const uint8_t version, const uint32_t bytesConsumed, const uint32_t totalBytes) {
  const bool asPartial = (version == SECTION_FILE_PARTIAL_VERSION);

  const auto failCommit = [this]() {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
    return false;
  };

  const uint32_t lutOffset = file.position();
  for (const auto& entry : build_->lut) {
    if (entry.fileOffset == 0) {
      LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
      return failCommit();
    }
    serialization::writePod(file, entry.fileOffset);
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets). For a
  // partial, skip anchors that landed on the incomplete trailing page the suspend drops.
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = build_->parser->getAnchors();
  size_t validAnchorCount = 0;
  for (const auto& [anchor, page] : anchors) {
    if (anchor.empty()) continue;
    if (page >= builtPageCount_) {
      if (asPartial) continue;
      LOG_ERR("SCT", "Failed to write anchor outside page range");
      return failCommit();
    }
    ++validAnchorCount;
    if (validAnchorCount > UINT16_MAX) {
      LOG_ERR("SCT", "Failed to write too many anchors");
      return failCommit();
    }
  }
  const uint16_t anchorCount = static_cast<uint16_t>(validAnchorCount);
  serialization::writePod(file, anchorCount);
  for (const auto& [anchor, page] : anchors) {
    if (anchor.empty() || (asPartial && page >= builtPageCount_)) continue;
    serialization::writeString(file, anchor);
    serialization::writePod(file, page);
  }

  const uint32_t paragraphLutOffset = file.position();
  serialization::writePod(file, static_cast<uint16_t>(build_->lut.size()));
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.paragraphIndex);
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (const auto& entry : build_->lut) {
    serialization::writePod(file, entry.listItemIndex);
  }

  if (asPartial) {
    // Watermark trailer, located on load as liLutOffset + pageCount * sizeof(uint16_t).
    serialization::writePod(file, bytesConsumed);
    serialization::writePod(file, totalBytes);
  }

  // Patch header with the built page count and section offsets...
  file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(builtPageCount_));
  serialization::writePod(file, builtPageCount_);
  serialization::writePod(file, lutOffset);
  serialization::writePod(file, anchorMapOffset);
  serialization::writePod(file, paragraphLutOffset);
  serialization::writePod(file, liLutFileOffset);
  // ...then commit by overwriting the sentinel version with the real one. Writing the
  // version last makes it the commit point: a crash before here leaves version 0.
  file.seek(0);
  serialization::writePod(file, version);
  // Explicit close() required: member variable persists beyond function scope
  file.close();

  // Swap into place. A crash between remove and rename loses the old file but keeps a
  // fully-committed tmp; the next build just removes it and rebuilds.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(binTmpPath().c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to move built section into place");
    Storage.remove(binTmpPath().c_str());
    return false;
  }

  HalFile committedFile;
  SectionCacheValidation::Layout validated;
  const bool opened = Storage.openFileForRead("SCT", filePath, committedFile);
  const bool verified = opened && validateSectionCache(committedFile, validated) && validated.version == version &&
                        validated.pageCount == builtPageCount_;
  if (opened) committedFile.close();
  if (!verified) {
    LOG_ERR("SCT", "Committed section failed structural verification");
    Storage.remove(filePath.c_str());
    cacheLayout_ = {};
    cacheLayoutValid_ = false;
    return false;
  }
  cacheLayout_ = validated;
  cacheLayoutValid_ = true;
  return true;
}

bool Section::finalizeBuild() {
  // Flush the trailing page (emits the last page via the completePageFn into the LUT).
  build_->parser->finishParse();

  if (!build_->reusedHtml) {
    // Parse succeeded: promote the freshly unzipped HTML to the persistent cache so future
    // rebuilds skip zip inflation. If promotion fails, drop the temp -- the build still succeeded.
    if (!Storage.rename(build_->tmpHtmlPath.c_str(), build_->htmlPath.c_str())) {
      LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
      Storage.remove(build_->tmpHtmlPath.c_str());
    }
  }

  const bool committed = commitBuildFile(SECTION_FILE_VERSION, 0, 0);
  if (build_->cssParser) build_->cssParser->clear();
  build_.reset();
  if (!committed) {
    // commitBuildFile removed filePath before the failed swap, so nothing valid remains.
    partial_ = false;
    partialPageCount_ = 0;
    pageCount = 0;
    builtPageCount_ = 0;
    cacheLayout_ = {};
    cacheLayoutValid_ = false;
    return false;
  }
  buildComplete_ = true;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = builtPageCount_;
  return true;
}

void Section::suspendBuild() {
  if (!build_) return;

  // Only worth persisting if this build produced pages a pre-existing partial doesn't
  // already cover; otherwise keep the older (bigger) partial and just drop the tmp.
  const bool worthKeeping = builtPageCount_ > 0 && (!partial_ || builtPageCount_ > partialPageCount_);

  bool committed = false;
  if (worthKeeping) {
    // Capture the parse watermark and commit before tearing the parser down (the anchor
    // map is read from it). The incomplete trailing page is intentionally not flushed:
    // only fully laid-out pages are persisted, and the rebuild re-derives the rest.
    const uint32_t consumed = static_cast<uint32_t>(build_->parser->parseBytesConsumed());
    committed = commitBuildFile(SECTION_FILE_PARTIAL_VERSION, consumed, build_->totalBytes);
    if (committed) {
      partial_ = true;
      partialPageCount_ = builtPageCount_;
      partialBytesConsumed_ = consumed;
      partialTotalBytes_ = build_->totalBytes;
      LOG_INF("SCT", "Suspended build: %u pages persisted", builtPageCount_);
    }
  }

  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (!committed && file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  pageCount = partial_ ? partialPageCount_ : 0;
  builtPageCount_ = 0;
}

void Section::abandonBuild() {
  if (!build_) return;
  if (build_->parser) build_->parser->abortParse();
  if (build_->cssParser) build_->cssParser->clear();
  if (file) {
    // Explicit close() required before remove (member variable, O_RDWR handle).
    file.close();
    Storage.remove(binTmpPath().c_str());
  }
  // A parse error would recur against the same HTML, so drop any partial too -- resuming
  // from it would just re-enter the failing build every open.
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!build_->reusedHtml && Storage.exists(build_->tmpHtmlPath.c_str())) {
    Storage.remove(build_->tmpHtmlPath.c_str());
  }
  build_.reset();
  buildComplete_ = false;
  partial_ = false;
  partialPageCount_ = 0;
  pageCount = 0;
  builtPageCount_ = 0;
  cacheLayout_ = {};
  cacheLayoutValid_ = false;
}

std::unique_ptr<Page> Section::loadPageDuringBuild(const int page) {
  if (!build_ || page < 0 || page >= static_cast<int>(build_->lut.size()) || !file) {
    return nullptr;
  }
  const uint32_t pos = build_->lut[page].fileOffset;
  if (pos == 0) {
    return nullptr;
  }
  // The .bin is open O_RDWR for the build. Read the already-written page, then restore
  // the write cursor so the next onPageComplete keeps appending where it left off.
  const uint32_t writePos = file.position();
  const uint32_t pageEnd =
      page + 1 < static_cast<int>(build_->lut.size()) ? build_->lut[page + 1].fileOffset : writePos;
  if (pageEnd <= pos) return nullptr;
  BoundedFileReader reader(file, pos, pageEnd);
  auto p = Page::deserialize(reader);
  file.seek(writePos);
  return p;
}

// Read a page from the committed file at filePath (finalized section or partial from a
// previous session). Uses a local handle so it is safe while a build holds the member
// `file` open on the tmp .bin.
std::unique_ptr<Page> Section::loadPageAt(const int page) const {
  if (!cacheLayoutValid_ || page < 0 || page >= cacheLayout_.pageCount) return nullptr;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return nullptr;
  }

  HalFileValidationInput input(f);
  uint64_t pageBegin = 0;
  uint64_t pageEnd = 0;
  if (!SectionCacheValidation::pageBounds(input, cacheLayout_, static_cast<uint16_t>(page), pageBegin, pageEnd)) {
    return nullptr;
  }
  BoundedFileReader reader(f, pageBegin, pageEnd);
  return Page::deserialize(reader);
  // No f.close() needed -- DESTRUCTOR_CLOSES_FILE=1 handles it at scope exit
}

std::unique_ptr<Page> Section::loadPage(const int page) {
  if (page < 0) {
    return nullptr;
  }
  if (build_ && page < static_cast<int>(build_->lut.size())) {
    return loadPageDuringBuild(page);
  }
  // Not (yet) in the active build: serve from the file on disk -- a finalized section,
  // or a partial from a previous session whose pages the rebuild hasn't reached again.
  const int onDisk = partial_ ? partialPageCount_ : (build_ ? 0 : pageCount);
  if (page >= onDisk) {
    return nullptr;
  }
  return loadPageAt(page);
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPage(currentPage);
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  return getCachedPageCount(epub->getCachePath(), spineIndex);
}

std::optional<uint16_t> Section::getCachedPageCount(const std::string& cachePath, const int spineIndex) {
  if (cachePath.empty() || spineIndex < 0) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", cachePath + "/sections/" + std::to_string(spineIndex) + ".bin", f)) {
    return std::nullopt;
  }

  SectionCacheValidation::Layout validated;
  const bool valid = validateSectionCache(f, validated) && !validated.partial && validated.pageCount > 0;
  const bool closed = f.close();
  return valid && closed ? std::optional<uint16_t>{validated.pageCount} : std::nullopt;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  if (!cacheLayoutValid_ || anchor.empty()) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (f.fileSize64() != cacheLayout_.fileSize) return std::nullopt;
  BoundedFileReader reader(f, cacheLayout_.anchorMapOffset, cacheLayout_.paragraphLutOffset);
  uint16_t count = 0;
  if (!reader.readPod(count)) return std::nullopt;
  for (uint16_t i = 0; i < count; i++) {
    uint32_t length = 0;
    uint16_t page = 0;
    bool matches = false;
    if (!reader.readPod(length) || length == 0 || length > SectionCacheValidation::MAX_ANCHOR_BYTES ||
        reader.remaining() < sizeof(uint16_t) || length > reader.remaining() - sizeof(uint16_t) ||
        !readStringEquals(reader, length, anchor, matches) || !reader.readPod(page) || page >= cacheLayout_.pageCount) {
      return std::nullopt;
    }
    if (matches) return page;
  }
  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  if (!cacheLayoutValid_ || cacheLayout_.pageCount == 0) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (f.fileSize64() != cacheLayout_.fileSize) return std::nullopt;
  BoundedFileReader reader(f, cacheLayout_.paragraphLutOffset, cacheLayout_.listItemLutOffset);
  uint16_t count = 0;
  if (!reader.readPod(count) || count != cacheLayout_.pageCount) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx = 0;
    if (!reader.readPod(pagePIdx)) return std::nullopt;
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getUniquePageForParagraphIndex(const uint16_t pIndex) const {
  if (!cacheLayoutValid_ || cacheLayout_.pageCount == 0) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) return std::nullopt;

  if (f.fileSize64() != cacheLayout_.fileSize) return std::nullopt;
  BoundedFileReader reader(f, cacheLayout_.paragraphLutOffset, cacheLayout_.listItemLutOffset);
  uint16_t count = 0;
  if (!reader.readPod(count) || count != cacheLayout_.pageCount) return std::nullopt;

  std::optional<uint16_t> result;
  for (uint16_t page = 0; page < count; ++page) {
    uint16_t pageParagraphIndex = 0;
    if (!reader.readPod(pageParagraphIndex)) return std::nullopt;
    if (pageParagraphIndex != pIndex) continue;
    if (result.has_value()) return std::nullopt;
    result = page;
  }
  return reader.atEnd() ? result : std::nullopt;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  if (!cacheLayoutValid_ || page >= cacheLayout_.pageCount) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (f.fileSize64() != cacheLayout_.fileSize) return std::nullopt;
  BoundedFileReader reader(f, cacheLayout_.paragraphLutOffset, cacheLayout_.listItemLutOffset);
  uint16_t count = 0;
  if (!reader.readPod(count) || count != cacheLayout_.pageCount ||
      !reader.skip(static_cast<uint64_t>(page) * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx = 0;
  if (!reader.readPod(pIdx)) return std::nullopt;
  return pIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  if (!cacheLayoutValid_ || cacheLayout_.pageCount == 0) return std::nullopt;
  HalFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  if (f.fileSize64() != cacheLayout_.fileSize) return std::nullopt;
  const uint64_t listItemEnd = static_cast<uint64_t>(cacheLayout_.listItemLutOffset) +
                               static_cast<uint64_t>(cacheLayout_.pageCount) * sizeof(uint16_t);
  BoundedFileReader reader(f, cacheLayout_.listItemLutOffset, listItemEnd);
  const uint16_t count = cacheLayout_.pageCount;
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx = 0;
    if (!reader.readPod(pageLiIdx)) return std::nullopt;
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
