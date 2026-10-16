#include "SdCardFontManager.h"

#include <EpdFontFamily.h>
#include <EspNorFlash.h>
#include <FlashFontPartition.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <SdCardFont.h>
#include <SdCardFontRegistry.h>
#include <ttf/TtfFamily.h>
#include <ttf/TtfFont.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

SdCardFontManager::SdCardFontManager() = default;

SdCardFontManager::~SdCardFontManager() {
  if (renderer_) {
    unloadAll(*renderer_);
  } else {
    for (auto& lf : loaded_) {
      delete lf.font;
    }
    loaded_.clear();
    ttf_.reset();
    ttfFlash_.reset();
  }
}

// FNV-1a continuation: seeds with contentHash, then hashes family name + point size.
int SdCardFontManager::computeFontId(uint32_t contentHash, const char* familyName, uint8_t pointSize) {
  static constexpr uint32_t FNV_PRIME = 16777619u;
  uint32_t hash = contentHash;
  while (*familyName) {
    hash ^= static_cast<uint8_t>(*familyName++);
    hash *= FNV_PRIME;
  }
  hash ^= pointSize;
  hash *= FNV_PRIME;
  int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

// Try to write the whole family (all sizes) to the flash partition.
// If the total size exceeds the available space, falls back to writing only
// the single requested file. Returns true if at least the requested size
// was written successfully.
static bool writeFamily(const SdCardFontFamilyInfo& family, uint8_t requestedPointSize) {
  // Collect available sizes and sort ascending so we write small→large.
  std::vector<uint8_t> sizes = family.availableSizes();
  std::sort(sizes.begin(), sizes.end());

  // Check whether the whole family fits.
  // FlashFontPartition::beginWrite erases everything below the language slot,
  // so we compute the total first without writing.
  {
    // Rough total: sum all file sizes + HEADER_BYTES overhead.
    size_t total = FlashFontPartition::HEADER_BYTES;
    bool allFit = true;
    // Budget against the partition's real ceiling rather than a hardcoded
    // figure. This used to be a conservative 3300 KB constant with a comment
    // saying the partition size could not be read here; fontUsableSize() now
    // reports it, and crucially it EXCLUDES the language pack slot at the tail
    // — a constant would have to be remembered and re-tuned whenever that
    // reservation changes.
    //
    // No extra slack is needed: the running total starts at the 4-byte-aligned
    // HEADER_BYTES and adds each file rounded up to 4, which is exactly what
    // appendFile() consumes. A missing partition reports 0 and nothing fits,
    // which correctly drops through to the single-file path below.
    const size_t partitionCap = FlashFontPartition::fontUsableSize();
    for (uint8_t sz : sizes) {
      const SdCardFontFileInfo* fi = family.findFile(sz);
      if (!fi) continue;
      HalFile f;
      if (!Storage.openFileForRead("FFP", fi->path.c_str(), f) || !f) continue;
      total += (f.fileSize() + 3) & ~static_cast<size_t>(3);  // 4-byte align
      f.close();
      if (total > partitionCap || static_cast<uint8_t>(sizes.size()) > FlashFontPartition::MAX_ENTRIES) {
        allFit = false;
        break;
      }
    }

    if (allFit && sizes.size() > 1) {
      // Write the whole family.
      if (!FlashFontPartition::beginWrite(family.name.c_str())) return false;
      bool requestedWritten = false;
      for (uint8_t sz : sizes) {
        const SdCardFontFileInfo* fi = family.findFile(sz);
        if (!fi) continue;
        if (FlashFontPartition::appendFile(fi->path.c_str(), family.name.c_str(), sz)) {
          if (sz == requestedPointSize) requestedWritten = true;
        } else {
          LOG_ERR("FFP", "appendFile failed for %s@%u; aborting family write", family.name.c_str(), sz);
          // Fall through to single-file fallback below.
          FlashFontPartition::finaliseWrite();  // attempt to commit what we have
          return requestedWritten;
        }
      }
      if (!FlashFontPartition::finaliseWrite()) return false;
      LOG_INF("SDMGR", "Cached full family %s (%zu sizes) to flash", family.name.c_str(), sizes.size());
      return requestedWritten;
    }
  }

  // Single-file fallback: only write the requested size.
  const SdCardFontFileInfo* fi = family.findFile(requestedPointSize);
  if (!fi) return false;
  if (!FlashFontPartition::beginWrite(family.name.c_str())) return false;
  if (!FlashFontPartition::appendFile(fi->path.c_str(), family.name.c_str(), requestedPointSize)) {
    FlashFontPartition::finaliseWrite();
    return false;
  }
  if (!FlashFontPartition::finaliseWrite()) return false;
  LOG_INF("SDMGR", "Cached single file %s@%u to flash", family.name.c_str(), requestedPointSize);
  return true;
}

bool SdCardFontManager::loadTtfFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer,
                                      const uint8_t targetPtSize, const std::function<void()>& onColdLoad) {
  // The partition has one owner (spec 4.1): a .cpfont mapping goes before the glyph store opens.
  if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();

  // The files stay open while the family loads: an install copies them into the store.
  HalFile files[4];
  ttf::FileSource sources[4];
  ttf::FileStamp stamps[4];
  for (const auto& file : family.files) {
    if (file.style >= 4) continue;
    HalFile& handle = files[file.style];
    if (!Storage.openFileForRead("SDMGR", file.path.c_str(), handle)) {
      LOG_ERR("SDMGR", "Cannot open %s", file.path.c_str());
      continue;
    }
    const auto size = static_cast<uint32_t>(handle.fileSize());
    sources[file.style] = ttf::fileSource(handle, size);
    uint16_t date = 0;
    uint16_t time = 0;
    handle.getModifyDateTime(&date, &time);
    const char* slash = std::strrchr(file.path.c_str(), '/');
    stamps[file.style] = {slash ? slash + 1 : file.path.c_str(), size, date, time};
  }
  uint8_t identity[glyphstore::kIdentityBytes];
  ttf::familyIdentity(stamps, identity);

  // Held while the family is loaded: the font area (a few words), and the font with its scratch block.
  auto flash = makeUniqueNoThrow<EspNorFlash>();
  if (!flash || !flash->begin()) {
    LOG_ERR("SDMGR", "No font partition for TTF family %s", family.name.c_str());
    return false;
  }
  auto font = makeUniqueNoThrow<TtfFont>(*flash);
  if (!font) {
    LOG_ERR("SDMGR", "OOM allocating TtfFont for %s", family.name.c_str());
    return false;
  }
  if (!font->load(sources, identity, onColdLoad) || !font->setBodySize(targetPtSize)) {
    LOG_ERR("SDMGR", "TTF family %s cannot be drawn at %u pt", family.name.c_str(), targetPtSize);
    return false;
  }
  const uint32_t identityHash = ttf::identityHash(identity);
  const int fontId = computeFontId(identityHash, family.name.c_str(), targetPtSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, family.name.c_str());
    return false;
  }
  ttfFlash_ = std::move(flash);
  ttf_ = std::move(font);
  ttfIdentityHash_ = identityHash;
  registerTtfFace(renderer, fontId);
  ttfNativeBase_ = pickNativeBase(renderer, identityHash, family.name.c_str());
  if (ttfNativeBase_ != 0) renderer.setFontResolver(&SdCardFontManager::resolveTtfSize, this);
  loadedFamilyName_ = family.name;
  loadedPointSize_ = targetPtSize;
  LOG_INF("SDMGR", "TTF family %s at %u pt (id=%d)", family.name.c_str(), targetPtSize, fontId);
  return true;
}

void SdCardFontManager::registerTtfFace(GfxRenderer& renderer, const int fontId) {
  // Not an SD font to the renderer: TtfFont keeps no per-page state for its maintenance loops.
  renderer.insertFont(fontId, EpdFontFamily(ttf_->font(0), ttf_->font(1), ttf_->font(2), ttf_->font(3)));
  ttfFontId_ = fontId;
}

int32_t SdCardFontManager::pickNativeBase(const GfxRenderer& renderer, const uint32_t identityHash,
                                          const char* familyName) const {
  // The window's high bits come from the family, so a section laid out with its sizes finds them again in the next
  // session; a window that holds a registered font ID moves on (another salt), so a size never shadows a font.
  for (uint8_t salt = 0; salt < 8; ++salt) {
    const int32_t base =
        ttf::nativeBaseFrom(static_cast<uint32_t>(computeFontId(identityHash, familyName, 0)) + salt * 0x100u);
    if (base == 0) continue;
    bool taken = false;
    for (const auto& entry : renderer.getFontMap()) {
      if (ttf::isNativeId(base, entry.first)) {
        taken = true;
        break;
      }
    }
    if (!taken) return base;
  }
  LOG_ERR("SDMGR", "%s: no free font ID window; sizes other than the body are scaled", familyName);
  return 0;
}

bool SdCardFontManager::fontIdTaken(const GfxRenderer& renderer, const int fontId) const {
  return renderer.getFontMap().count(fontId) != 0 || ttf::isNativeId(ttfNativeBase_, fontId);
}

std::optional<ResolvedFont<EpdFontFamily>> SdCardFontManager::resolveTtfSize(void* ctx, const int fontId) {
  // Asked for every font ID the renderer does not know: declining is one comparison.
  auto* self = static_cast<SdCardFontManager*>(ctx);
  if (!self->ttf_ || !ttf::isNativeId(self->ttfNativeBase_, fontId)) return std::nullopt;
  const uint8_t px = ttf::nativeIdPixels(fontId);
  const EpdFont* fonts[4] = {};
  uint32_t served = 0;
  if (!self->ttf_->nativeFonts(px, fonts, &served)) {
    LOG_ERR("SDMGR", "%u px cannot be drawn natively; the body face serves", px);
    for (uint8_t s = 0; s < 4; ++s) fonts[s] = self->ttf_->font(s);
    served = self->ttf_->bodyPpem();
  }
  // A stand-in (the nearest size held, or the body) is scaled to `px`: a page laid out with this ID in another
  // session, where the size itself served, keeps its measurements.
  ResolvedFont<EpdFontFamily> resolved{EpdFontFamily(fonts[0], fonts[1], fonts[2], fonts[3])};
  const uint32_t wanted = static_cast<uint32_t>(px) << 6;
  if (served != 0 && served != wanted) {
    resolved.baseScale = static_cast<float>(wanted) / static_cast<float>(served);
    LOG_INF("SDMGR", "%u px drawn from the %lu/64 px size, scaled", px, static_cast<unsigned long>(served));
  }
  return resolved;
}

std::optional<SdCardFontManager::NativeSizes> SdCardFontManager::ttfNativeSizes(const int bodyFontId) const {
  if (!ttf_ || ttfNativeBase_ == 0 || bodyFontId != ttfFontId_) return std::nullopt;
  NativeSizes sizes;
  sizes.base = ttfNativeBase_;
  sizes.bodyPpem = ttf_->bodyPpem();
  sizes.minPx = TtfFont::kMinNativePx;
  sizes.maxPx = ttf_->maxNativePx();
  return sizes;
}

void SdCardFontManager::dropTtfNativeSizes(GfxRenderer& renderer) {
  if (!ttf_) return;
  // One at a time: removeFont() ends the walk over the map.
  for (bool found = true; found;) {
    found = false;
    for (const auto& entry : renderer.getFontMap()) {
      if (!ttf::isNativeId(ttfNativeBase_, entry.first)) continue;
      renderer.removeFont(entry.first);
      found = true;
      break;
    }
  }
  ttf_->dropNativeSizes();
}

bool SdCardFontManager::setTtfBodySize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttf_) return false;
  if (pointSize == loadedPointSize_) return true;
  const int fontId = computeFontId(ttfIdentityHash_, loadedFamilyName_.c_str(), pointSize);
  if (fontIdTaken(renderer, fontId)) {
    LOG_ERR("SDMGR", "Font ID %d for %s@%u collides with a registered font", fontId, loadedFamilyName_.c_str(),
            pointSize);
    return false;
  }
  // A native size the body stood in for was scaled from the old body size: all of them go, and come back on demand.
  dropTtfNativeSizes(renderer);
  if (!ttf_->setBodySize(pointSize)) return false;
  renderer.removeFont(ttfFontId_);
  registerTtfFace(renderer, fontId);
  loadedPointSize_ = pointSize;
  LOG_INF("SDMGR", "TTF family %s now at %u pt (id=%d)", loadedFamilyName_.c_str(), pointSize, fontId);
  return true;
}

void SdCardFontManager::wipeTtfIfPending(const std::function<void()>& onBusy) {
  if (ttf_) ttf_->wipeIfPending(onBusy);
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                                   const std::function<void()>& onColdLoad, const FlashCachePolicy policy,
                                   BuildArena* const arena) {
  if (!renderer_) renderer_ = &renderer;
  if (!loadedFamilyName_.empty()) unloadAll(renderer);

  if (family.ttf()) {
    // A preview must never install over the partition; TTF previews come later (spec 7.6).
    if (policy == FlashCachePolicy::ReadOnly) return false;
    return loadTtfFamily(family, renderer, targetPtSize, onColdLoad);
  }

  const SdCardFontFileInfo* selected = family.pickClosestSize(targetPtSize);
  if (!selected) {
    LOG_ERR("SDMGR", "Family %s has no files to load", family.name.c_str());
    return false;
  }

  auto* font = new (std::nothrow) SdCardFont();
  if (!font) {
    LOG_ERR("SDMGR", "OOM allocating SdCardFont for %s", selected->path.c_str());
    return false;
  }
  font->useArena(arena);

  // --- Flash partition cache ---
  // If the partition already has this exact entry, mmap directly — no write
  // needed. Otherwise write the family (or single file) and then mmap.
  bool loadedFromMmap = false;
  {
    if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();

    const bool alreadyCached = FlashFontPartition::hasEntry(family.name.c_str(), selected->pointSize);

    bool readyToMmap = alreadyCached;
    if (!alreadyCached && policy == FlashCachePolicy::ReadOnly) {
      // Transient load (font preview): beginWrite() erases the whole partition
      // and writeFamily() copies every size back from SD, so caching a font the
      // reader may never use would cost seconds and a flash erase cycle each
      // time. Read the one file from SD instead and leave the partition alone.
      LOG_DBG("SDMGR", "Flash cache miss for %s@%u; loading from SD (read-only policy)", family.name.c_str(),
              selected->pointSize);
    } else if (!alreadyCached) {
      // Genuine first load: writing the family into the flash partition takes
      // a noticeable moment — let the caller advertise it before we start.
      if (onColdLoad) onColdLoad();
      readyToMmap = writeFamily(family, selected->pointSize);
      if (!readyToMmap) {
        LOG_ERR("SDMGR", "Flash write failed for %s, falling back to SD load", family.name.c_str());
      }
    } else {
      LOG_DBG("SDMGR", "Flash cache hit for %s@%u", family.name.c_str(), selected->pointSize);
    }

    if (readyToMmap) {
      const uint8_t* mmapPtr = nullptr;
      size_t mmapSize = 0;
      if (FlashFontPartition::mmap(family.name.c_str(), selected->pointSize, &mmapPtr, &mmapSize)) {
        if (font->loadFromMmap(mmapPtr, mmapSize, selected->path.c_str())) {
          loadedFromMmap = true;
          LOG_INF("SDMGR", "Font loaded from flash: %s@%u", family.name.c_str(), selected->pointSize);
        } else {
          LOG_ERR("SDMGR", "loadFromMmap failed, falling back to SD");
          FlashFontPartition::unmap();
        }
      } else {
        LOG_ERR("SDMGR", "mmap failed for %s@%u, falling back to SD", family.name.c_str(), selected->pointSize);
      }
    }
  }

  if (!loadedFromMmap) {
    if (!font->load(selected->path.c_str())) {
      LOG_ERR("SDMGR", "Failed to load %s", selected->path.c_str());
      delete font;
      return false;
    }
  }

  int fontId = computeFontId(font->contentHash(), family.name.c_str(), selected->pointSize);
  if (renderer.getFontMap().count(fontId) != 0) {
    LOG_ERR("SDMGR", "Font ID %d collides with existing font, skipping %s", fontId, selected->path.c_str());
    delete font;
    return false;
  }
  renderer.registerSdCardFont(fontId, font);
  loaded_.push_back({font, fontId, selected->pointSize});

  LOG_DBG("SDMGR", "Loaded %s size=%u id=%d styles=%u (target=%u)", selected->path.c_str(), selected->pointSize, fontId,
          font->styleCount(), targetPtSize);

  EpdFontFamily fontFamily(font->getEpdFont(0), font->getEpdFont(1), font->getEpdFont(2), font->getEpdFont(3));
  renderer.insertFont(fontId, fontFamily);

  loadedFamilyName_ = family.name;
  loadedPointSize_ = selected->pointSize;
  // A failure here is logged and leaves the target size unresolvable; the face itself is still
  // loaded and usable at its own size, so it is not a failed load.
  ensureSizeAlias(renderer, targetPtSize);
  return true;
}

bool SdCardFontManager::ensureSizeAlias(GfxRenderer& renderer, const uint8_t targetPtSize) {
  if (loaded_.empty() || loadedPointSize_ == 0 || targetPtSize == 0) return false;
  if (!renderer_) renderer_ = &renderer;

  if (targetPtSize == loadedPointSize_) {
    dropSizeAlias(renderer);  // the face serves this size itself
    return true;
  }
  if (aliasFontId_ != 0 && aliasPointSize_ == targetPtSize) return true;
  dropSizeAlias(renderer);

  const LoadedFont& lf = loaded_.front();
  const int aliasId = computeFontId(lf.font->contentHash(), loadedFamilyName_.c_str(), targetPtSize);
  if (renderer.getFontMap().count(aliasId) != 0) {
    LOG_ERR("SDMGR", "Alias ID %d for %s@%u collides with a registered font; that size stays unavailable", aliasId,
            loadedFamilyName_.c_str(), targetPtSize);
    return false;
  }

  // Same four faces as the native ID: the alias differs only in the base scale the renderer
  // applies. Registered as an SD alias too, so ensureFontReady() and the prewarm scan find the
  // SdCardFont behind it -- a scaled ID whose glyphs are never prewarmed thrashes the 8-slot
  // overflow ring at ~12 ms a miss.
  EpdFontFamily fontFamily(lf.font->getEpdFont(0), lf.font->getEpdFont(1), lf.font->getEpdFont(2),
                           lf.font->getEpdFont(3));
  renderer.registerSdCardFontAlias(aliasId, lf.font);
  renderer.insertScaledFont(aliasId, fontFamily, static_cast<float>(targetPtSize) / loadedPointSize_);
  aliasFontId_ = aliasId;
  aliasPointSize_ = targetPtSize;
  LOG_INF("SDMGR", "%s: %u pt served by the %u pt face scaled %u/%u (id=%d)", loadedFamilyName_.c_str(), targetPtSize,
          loadedPointSize_, targetPtSize, loadedPointSize_, aliasId);
  return true;
}

void SdCardFontManager::dropSizeAlias(GfxRenderer& renderer) {
  if (aliasFontId_ == 0) return;
  renderer.removeFont(aliasFontId_);  // also drops its base scale
  renderer.unregisterSdCardFont(aliasFontId_);
  aliasFontId_ = 0;
  aliasPointSize_ = 0;
}

void SdCardFontManager::unloadAll(GfxRenderer& renderer) {
  if (!renderer_) renderer_ = &renderer;
  if (ttf_) {
    dropTtfNativeSizes(renderer);
    renderer.setFontResolver(nullptr, nullptr);
    ttfNativeBase_ = 0;
    renderer.removeFont(ttfFontId_);
    LOG_INF("SDMGR",
            "TTF family %s unloaded: %lu glyphs rendered, %lu read from the store, %lu kerning pairs worked out",
            loadedFamilyName_.c_str(), static_cast<unsigned long>(ttf_->renders()),
            static_cast<unsigned long>(ttf_->storeHits()), static_cast<unsigned long>(ttf_->kernLookups()));
    ttf_.reset();  // closes the store before its flash goes
    ttfFlash_.reset();
    ttfFontId_ = 0;
  }
  dropSizeAlias(renderer);
  renderer.clearSdCardFonts();
  for (auto& lf : loaded_) {
    renderer.removeFont(lf.fontId);
    delete lf.font;
  }
  loaded_.clear();
  loadedFamilyName_.clear();
  loadedPointSize_ = 0;
  if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();
}

int SdCardFontManager::getFontId(const std::string& familyName, const uint8_t pointSize) const {
  if (ttf_) return familyName == loadedFamilyName_ && pointSize == loadedPointSize_ ? ttfFontId_ : 0;
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  if (pointSize == loadedPointSize_) return loaded_.front().fontId;
  if (aliasFontId_ != 0 && pointSize == aliasPointSize_) return aliasFontId_;
  return 0;
}
