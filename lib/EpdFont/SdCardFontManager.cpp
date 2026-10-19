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
    ttfSet_.reset();
    ttfSources_.reset();
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

// What a load holds while the set installs: every family's style files, open (an install copies them into the
// store), and each family's identity. On the heap: several families' handles and sources would crowd the frame. Kept
// while a family is in SD mode, which reads its outlines through these files; the other families' files are closed.
struct SdCardFontManager::TtfSources {
  HalFile files[kMaxTtfFamilies][4];
  ttf::FileSource sources[kMaxTtfFamilies][4];
  uint8_t identities[kMaxTtfFamilies][glyphstore::kIdentityBytes];
};

namespace {

static_assert(SdCardFontManager::kMaxTtfFamilies <= TtfFontSet::kMaxFamilies, "the set holds the manager's families");

// Opens a family's style files, and gives the family's identity from what the card says of them.
void openTtfFiles(const SdCardFontFamilyInfo& family, HalFile (&files)[4], ttf::FileSource (&sources)[4],
                  uint8_t (&identity)[glyphstore::kIdentityBytes]) {
  ttf::FileStamp stamps[4];
  for (const auto& file : family.files) {
    if (file.style >= 4) continue;
    HalFile& handle = files[file.style];
    if (!Storage.openFileForRead("SDMGR", file.path.c_str(), handle)) {
      LOG_ERR("SDMGR", "Cannot open %s", file.path.c_str());
      continue;
    }
    const auto size = static_cast<uint32_t>(handle.fileSize());
    uint16_t date = 0;
    uint16_t time = 0;
    handle.getModifyDateTime(&date, &time);
    // The card's stamp of the file, which an SD-mode store keeps (FileSource::stamp): its path and time.
    const uint32_t when = static_cast<uint32_t>(date) << 16 | time;
    const uint32_t stamp =
        glyphstore::fnv1a(&when, sizeof when, glyphstore::fnv1a(file.path.c_str(), file.path.size()));
    sources[file.style] = ttf::fileSource(handle, size, stamp);
    const char* slash = std::strrchr(file.path.c_str(), '/');
    stamps[file.style] = {slash ? slash + 1 : file.path.c_str(), size, date, time};
  }
  ttf::familyIdentity(stamps, identity);
}

// The first `count` families as one set, under the set's identity; nullptr when they cannot be drawn together.
std::unique_ptr<TtfFontSet> loadSet(NorFlash& flash, const ttf::FileSource (*sources)[4],
                                    const uint8_t (*identities)[glyphstore::kIdentityBytes], const uint8_t count,
                                    const std::function<void()>& onColdLoad, const TtfFontSet::Install install) {
  uint8_t identity[glyphstore::kIdentityBytes];
  ttf::setIdentity(identities, count, identity);
  auto set = makeUniqueNoThrow<TtfFontSet>(flash);
  if (!set || !set->load(sources, count, identity, onColdLoad, install)) return nullptr;
  return set;
}

// Which families a load was asked for, in their order.
uint32_t askedKey(const SdCardFontFamilyInfo* const* families, const uint8_t count) {
  uint32_t key = glyphstore::fnv1a(&count, 1);
  for (uint8_t f = 0; f < count; ++f) {
    const uint32_t files = families[f]->filesKey();
    key = glyphstore::fnv1a(&files, sizeof files, key);
  }
  return key;
}

}  // namespace

bool SdCardFontManager::loadTtfFamilies(const SdCardFontFamilyInfo* const* families, const uint8_t count,
                                        GfxRenderer& renderer, const uint8_t targetPtSize,
                                        const std::function<void()>& onColdLoad) {
  return loadTtf(families, count, renderer, targetPtSize, onColdLoad, TtfChoice::FewerIfNeeded);
}

bool SdCardFontManager::loadTtf(const SdCardFontFamilyInfo* const* families, uint8_t count, GfxRenderer& renderer,
                                const uint8_t targetPtSize, const std::function<void()>& onColdLoad,
                                const TtfChoice choice) {
  if (!renderer_) renderer_ = &renderer;
  if (!loadedFamilyName_.empty()) unloadAll(renderer);
  if (count == 0) return false;
  if (count > kMaxTtfFamilies) count = kMaxTtfFamilies;
  // The partition has one owner (spec 4.1): a .cpfont mapping goes before the glyph store opens.
  if (FlashFontPartition::isMapped()) FlashFontPartition::unmap();

  // Held while the families are loaded: the font area (a few words), and the set with its one scratch block.
  auto flash = makeUniqueNoThrow<EspNorFlash>();
  auto opened = makeUniqueNoThrow<TtfSources>();
  if (!flash || !opened || !flash->begin()) {
    LOG_ERR("SDMGR", "No font partition, or no memory, for TTF family %s", families[0]->name.c_str());
    return false;
  }
  for (uint8_t f = 0; f < count; ++f) {
    openTtfFiles(*families[f], opened->files[f], opened->sources[f], opened->identities[f]);
  }
  // Exactly these families are a set the store holds already: never installed then (see resumeTtf()).
  const TtfFontSet::Install install =
      choice == TtfChoice::ExactlyThese ? TtfFontSet::Install::Never : TtfFontSet::Install::IfNeeded;
  uint8_t loaded = count;
  auto set = loadSet(*flash, opened->sources, opened->identities, count, onColdLoad, install);
  while (!set && loaded > 1 && choice == TtfChoice::FewerIfNeeded) {
    // Too large together, or an accent family that does not read: without the last family, the least used accent,
    // and so on down to the body's family alone, as before Stage B.
    --loaded;
    LOG_INF("SDMGR", "TTF families do not load together; %s left out", families[loaded]->name.c_str());
    set = loadSet(*flash, opened->sources, opened->identities, loaded, onColdLoad, install);
  }
  if (!set) {
    LOG_ERR("SDMGR", "TTF family %s cannot be drawn", families[0]->name.c_str());
    return false;
  }
  ttfFlash_ = std::move(flash);
  ttfSet_ = std::move(set);
  for (uint8_t f = 0; f < loaded; ++f) {
    const char* name = families[f]->name.c_str();
    const uint32_t identityHash = ttf::identityHash(opened->identities[f]);
    const int fontId = computeFontId(identityHash, name, targetPtSize);
    if (!ttfSet_->font(f)->setBodySize(targetPtSize) || renderer.getFontMap().count(fontId) != 0) {
      LOG_ERR("SDMGR", "TTF family %s cannot be drawn at %u pt (id=%d)", name, targetPtSize, fontId);
      break;
    }
    TtfFamily& family = ttfFamilies_[ttfCount_++];
    family.name = families[f]->name;
    family.identityHash = identityHash;
    registerTtfFace(renderer, f, fontId);
    family.nativeBase = pickNativeBase(renderer, identityHash, name);
    LOG_INF("SDMGR", "TTF family %s at %u pt (id=%d)", name, targetPtSize, fontId);
  }
  if (ttfCount_ == 0) {
    ttfSet_.reset();
    ttfFlash_.reset();
    return false;
  }
  // The files of a family in SD mode stay open while the set is loaded; the rest are closed now.
  bool onCard = false;
  for (uint8_t f = 0; f < count; ++f) {
    if (f < loaded && ttfSet_->font(f)->readsCard()) {
      onCard = true;
      continue;
    }
    for (HalFile& file : opened->files[f]) file = HalFile();  // closed, and its handle freed
  }
  if (onCard) ttfSources_ = std::move(opened);
  renderer.setFontResolver(&SdCardFontManager::resolveTtfSize, this);
  ttfAsked_ = askedKey(families, count);
  loadedFamilyName_ = ttfFamilies_[0].name;
  loadedPointSize_ = targetPtSize;
  return true;
}

bool SdCardFontManager::ttfFamiliesLoaded(const SdCardFontFamilyInfo* const* families, const uint8_t count) const {
  return ttfSet_ && count != 0 && count <= kMaxTtfFamilies && ttfAsked_ == askedKey(families, count);
}

void SdCardFontManager::registerTtfFace(GfxRenderer& renderer, const uint8_t family, const int fontId) {
  // Not an SD font to the renderer: TtfFont keeps no per-page state for its maintenance loops.
  const TtfFont& font = *ttfSet_->font(family);
  renderer.insertFont(fontId, EpdFontFamily(font.font(0), font.font(1), font.font(2), font.font(3)));
  ttfFamilies_[family].fontId = fontId;
}

int32_t SdCardFontManager::pickNativeBase(const GfxRenderer& renderer, const uint32_t identityHash,
                                          const char* familyName) const {
  // The window's high bits come from the family, so a section laid out with its sizes finds them again in the next
  // session; a window that holds a registered font ID, or is another family's, moves on (another salt), so a size
  // never shadows a font.
  for (uint8_t salt = 0; salt < 8; ++salt) {
    const int32_t base =
        ttf::nativeBaseFrom(static_cast<uint32_t>(computeFontId(identityHash, familyName, 0)) + salt * 0x100u);
    if (base == 0) continue;
    bool taken = ttfFamilyOfNativeId(base) >= 0;
    for (const auto& entry : renderer.getFontMap()) {
      if (taken) break;
      taken = ttf::isNativeId(base, entry.first);
    }
    if (!taken) return base;
  }
  LOG_ERR("SDMGR", "%s: no free font ID window; sizes other than the body are scaled", familyName);
  return 0;
}

int SdCardFontManager::ttfFamilyOfNativeId(const int fontId) const {
  for (uint8_t f = 0; f < ttfCount_; ++f) {
    if (ttf::isNativeId(ttfFamilies_[f].nativeBase, fontId)) return f;
  }
  return -1;
}

bool SdCardFontManager::fontIdTaken(const GfxRenderer& renderer, const int fontId) const {
  return renderer.getFontMap().count(fontId) != 0 || ttfFamilyOfNativeId(fontId) >= 0;
}

std::optional<ResolvedFont<EpdFontFamily>> SdCardFontManager::resolveTtfSize(void* ctx, const int fontId) {
  // Asked for every font ID the renderer does not know: declining is a comparison per family, and the window an ID
  // falls in says whose size it is.
  auto* self = static_cast<SdCardFontManager*>(ctx);
  const int family = self->ttfSet_ ? self->ttfFamilyOfNativeId(fontId) : -1;
  if (family < 0) return std::nullopt;
  TtfFont& font = *self->ttfSet_->font(static_cast<uint8_t>(family));
  const uint8_t px = ttf::nativeIdPixels(fontId);
  const EpdFont* fonts[4] = {};
  uint32_t served = 0;
  if (!font.nativeFonts(px, fonts, &served)) {
    LOG_ERR("SDMGR", "%u px cannot be drawn natively; the body face serves", px);
    for (uint8_t s = 0; s < 4; ++s) fonts[s] = font.font(s);
    served = font.bodyPpem();
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
  if (!ttfSet_) return std::nullopt;  // none loaded, or suspended
  for (uint8_t f = 0; f < ttfCount_; ++f) {
    const TtfFamily& family = ttfFamilies_[f];
    if (family.fontId != bodyFontId || family.nativeBase == 0) continue;
    const TtfFont& font = *ttfSet_->font(f);
    NativeSizes sizes;
    sizes.base = family.nativeBase;
    sizes.bodyPpem = font.bodyPpem();
    sizes.minPx = TtfFont::kMinNativePx;
    sizes.maxPx = font.maxNativePx();
    return sizes;
  }
  return std::nullopt;
}

void SdCardFontManager::dropTtfNativeSizes(GfxRenderer& renderer) {
  if (!ttfSet_) return;
  // One at a time: removeFont() ends the walk over the map.
  for (bool found = true; found;) {
    found = false;
    for (const auto& entry : renderer.getFontMap()) {
      if (ttfFamilyOfNativeId(entry.first) < 0) continue;
      renderer.removeFont(entry.first);
      found = true;
      break;
    }
  }
  for (uint8_t f = 0; f < ttfCount_; ++f) ttfSet_->font(f)->dropNativeSizes();
}

bool SdCardFontManager::setTtfBodySize(GfxRenderer& renderer, const uint8_t pointSize) {
  if (!ttfSet_) return false;
  if (pointSize == loadedPointSize_) return true;
  // A native size the body stood in for was scaled from the old body size: all of them go, and come back on demand.
  dropTtfNativeSizes(renderer);
  for (uint8_t f = 0; f < ttfCount_; ++f) {
    TtfFamily& family = ttfFamilies_[f];
    const int fontId = computeFontId(family.identityHash, family.name.c_str(), pointSize);
    if (fontId != family.fontId && fontIdTaken(renderer, fontId)) {
      LOG_ERR("SDMGR", "Font ID %d for %s@%u collides with a registered font", fontId, family.name.c_str(), pointSize);
      return false;
    }
    if (!ttfSet_->font(f)->setBodySize(pointSize)) return false;
    renderer.removeFont(family.fontId);
    registerTtfFace(renderer, f, fontId);
    LOG_INF("SDMGR", "TTF family %s now at %u pt (id=%d)", family.name.c_str(), pointSize, fontId);
  }
  loadedPointSize_ = pointSize;
  return true;
}

void SdCardFontManager::wipeTtfIfPending(const std::function<void()>& onBusy) {
  if (ttfSet_) ttfSet_->wipeIfPending(onBusy);
}

uint32_t SdCardFontManager::releaseTtfMissMemory() { return ttfSet_ ? ttfSet_->releaseMissMemory() : 0; }

uint32_t SdCardFontManager::ttfMissMemoryBytes() const { return ttfSet_ ? ttfSet_->missMemoryBytes() : 0; }

void SdCardFontManager::closeTtfSet(GfxRenderer& renderer) {
  dropTtfNativeSizes(renderer);
  renderer.setFontResolver(nullptr, nullptr);
  for (uint8_t f = 0; f < ttfCount_; ++f) renderer.removeFont(ttfFamilies_[f].fontId);
  ttfSet_.reset();  // closes the store before its flash goes, and stops reading before its files close
  ttfSources_.reset();
  ttfFlash_.reset();
}

bool SdCardFontManager::suspendTtf(GfxRenderer& renderer) {
  if (!ttfSet_) return false;
  closeTtfSet(renderer);
  ttfSuspended_ = true;
  LOG_INF("SDMGR", "TTF set of %u famil%s suspended", ttfCount_, ttfCount_ == 1 ? "y" : "ies");
  return true;
}

bool SdCardFontManager::resumeTtf(const SdCardFontFamilyInfo* const* families, GfxRenderer& renderer) {
  if (!ttfSuspended_) return ttfSet_ != nullptr;
  const uint8_t count = ttfCount_;
  // The IDs alone, not the names: the heap is at its shortest here.
  int fontIds[kMaxTtfFamilies] = {};
  int32_t nativeBases[kMaxTtfFamilies] = {};
  for (uint8_t f = 0; f < count; ++f) {
    fontIds[f] = ttfFamilies_[f].fontId;
    nativeBases[f] = ttfFamilies_[f].nativeBase;
  }
  const uint32_t asked = ttfAsked_;
  const uint8_t pointSize = loadedPointSize_;
  unloadAll(renderer);  // the suspended state: nothing of the set is left to free
  bool same = loadTtf(families, count, renderer, pointSize, {}, TtfChoice::ExactlyThese) && ttfCount_ == count;
  for (uint8_t f = 0; same && f < count; ++f) {
    same = ttfFamilies_[f].fontId == fontIds[f] && ttfFamilies_[f].nativeBase == nativeBases[f];
  }
  if (!same) {
    // Pages laid out with the old IDs would draw nothing in their place.
    if (ttfSet_) LOG_ERR("SDMGR", "TTF set loaded again under other font IDs; unloaded");
    unloadAll(renderer);
    return false;
  }
  ttfAsked_ = asked;  // what the load was asked for, the families left out at its time among them
  return true;
}

bool SdCardFontManager::loadFamily(const SdCardFontFamilyInfo& family, GfxRenderer& renderer, uint8_t targetPtSize,
                                   const std::function<void()>& onColdLoad, const FlashCachePolicy policy,
                                   BuildArena* const arena) {
  if (!renderer_) renderer_ = &renderer;
  if (!loadedFamilyName_.empty()) unloadAll(renderer);

  if (family.ttf()) {
    // A preview must never install over the partition; TTF previews come later (spec 7.6).
    if (policy == FlashCachePolicy::ReadOnly) return false;
    const SdCardFontFamilyInfo* const one = &family;
    return loadTtfFamilies(&one, 1, renderer, targetPtSize, onColdLoad);
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
  if (ttfSet_) {
    for (uint8_t f = 0; f < ttfCount_; ++f) {
      const TtfFont& font = *ttfSet_->font(f);
      LOG_INF("SDMGR",
              "TTF family %s unloaded: %lu glyphs rendered, %lu read from the store, %lu kerning pairs worked out%s",
              ttfFamilies_[f].name.c_str(), static_cast<unsigned long>(font.renders()),
              static_cast<unsigned long>(font.storeHits()), static_cast<unsigned long>(font.kernLookups()),
              font.readsCard() ? " (SD mode)" : "");
    }
    LOG_INF("SDMGR", "TTF set unloaded: %lu SD pages read", static_cast<unsigned long>(ttfSet_->sdPageReads()));
    closeTtfSet(renderer);
  }
  // A suspended set is gone already; what was kept for its resume goes now.
  for (uint8_t f = 0; f < ttfCount_; ++f) ttfFamilies_[f] = {};
  ttfCount_ = 0;
  ttfAsked_ = 0;
  ttfSuspended_ = false;
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
  if (ttfSet_) {
    for (uint8_t f = 0; f < ttfCount_ && pointSize == loadedPointSize_; ++f) {
      if (familyName == ttfFamilies_[f].name) return ttfFamilies_[f].fontId;
    }
    return 0;
  }
  if (familyName != loadedFamilyName_ || loaded_.empty()) return 0;
  if (pointSize == loadedPointSize_) return loaded_.front().fontId;
  if (aliasFontId_ != 0 && pointSize == aliasPointSize_) return aliasFontId_;
  return 0;
}
