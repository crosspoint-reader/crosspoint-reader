#include "BookFontSystem.h"

#if CROSSPOINT_VECTOR_FONTS

#include <Arduino.h>
#include <Epub.h>
#include <FontPsram.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <Logging.h>
#include <Memory.h>
#include <Print.h>
#include <TtfEpdFont.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cstring>

#include "CrossPointSettings.h"

BookFontSystem bookFontSystem;

namespace {

// Fonts past this are skipped: CJK faces belong on the SD font path, which streams.
constexpr size_t MAX_FONT_BYTES = 4 * 1024 * 1024;
// PSRAM left free after a font file is read in.
constexpr size_t PSRAM_HEADROOM = 512 * 1024;

// Print sink that fills a preallocated buffer, failing once it is full.
class BufferPrint final : public Print {
 public:
  BufferPrint(uint8_t* data, const size_t capacity) : data_(data), capacity_(capacity) {}
  size_t write(const uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t* buffer, const size_t size) override {
    if (size > capacity_ - used_) return 0;
    memcpy(data_ + used_, buffer, size);
    used_ += size;
    return size;
  }
  size_t used() const { return used_; }

 private:
  uint8_t* data_;
  size_t capacity_;
  size_t used_ = 0;
};

}  // namespace

BookFontSystem::FontBytes::~FontBytes() { freeink::font::psramDeleteArray(data); }

BookFontSystem::BookFontSystem() = default;
BookFontSystem::~BookFontSystem() = default;

void BookFontSystem::open(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer) {
  if (epub_ == epub) return;
  close();
  epub_ = epub;
  renderer_ = &renderer;
  loaded_.reserve(MAX_LOADED);

  std::vector<CssFontFace> faces;
  if (!CssParser::loadFontFaces(epub->getCachePath(), faces)) return;
  families_.reserve(faces.size());
  for (CssFontFace& face : faces) {
    auto family = std::find_if(families_.begin(), families_.end(),
                               [&face](const Family& candidate) { return candidate.hash == face.family; });
    if (family == families_.end()) {
      families_.emplace_back();
      family = families_.end() - 1;
      family->hash = face.family;
    }
    const uint8_t role = (face.bold ? TtfEpdFont::Bold : 0) | (face.italic ? TtfEpdFont::Italic : 0);
    if (family->hrefs[role].empty()) {
      family->hrefs[role] = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(face.href));
    }
  }
  // TtfEpdFont needs a regular source; a family declared only in other styles uses its first.
  for (Family& family : families_) {
    for (auto& href : family.hrefs) {
      if (family.hrefs[TtfEpdFont::Regular].empty() && !href.empty()) family.hrefs[TtfEpdFont::Regular] = href;
    }
  }
  LOG_DBG("BKF", "%u embedded font families", static_cast<unsigned>(families_.size()));
}

void BookFontSystem::close() {
  unloadFonts();
  families_.clear();
  epub_.reset();
}

void BookFontSystem::unloadFonts() {
  for (const auto& loaded : loaded_) {
    renderer_->unregisterTtfFont(loaded.fontId);
    renderer_->removeFont(loaded.fontId);
  }
  loaded_.clear();
}

int BookFontSystem::fontIdFor(const uint32_t family, const uint8_t pointSize) {
  uint32_t hash = 2166136261u;
  for (int shift = 0; shift < 32; shift += 8) {
    hash ^= static_cast<uint8_t>(family >> shift);
    hash *= 16777619u;
  }
  hash ^= pointSize;
  hash *= 16777619u;
  hash ^= 0x424B4600u;  // "BKF\0" salt keeps book fonts apart from reader and variant ids
  const int id = static_cast<int>(hash);
  return id != 0 ? id : 1;
}

int BookFontSystem::resolve(const uint32_t family, const float scale) {
  auto it = std::find_if(families_.begin(), families_.end(),
                         [family](const Family& candidate) { return candidate.hash == family; });
  if (it == families_.end()) return 0;
  const auto pointSize =
      static_cast<uint8_t>(std::clamp(static_cast<int>(SETTINGS.fontPointSize * scale + 0.5f), 6, 72));
  const int id = fontIdFor(family, pointSize);
  return loadFont(*it, pointSize, id) ? id : 0;
}

bool BookFontSystem::load(const int fontId) {
  for (Family& family : families_) {
    for (int pointSize = 6; pointSize <= 72; ++pointSize) {
      if (fontIdFor(family.hash, static_cast<uint8_t>(pointSize)) == fontId) {
        return loadFont(family, static_cast<uint8_t>(pointSize), fontId);
      }
    }
  }
  return false;
}

bool BookFontSystem::readFamilyBytes(Family& family) {
  if (family.bytesLoaded) return family.bytes[TtfEpdFont::Regular] != nullptr;
  family.bytesLoaded = true;
  for (int role = 0; role < 4; ++role) {
    const std::string& href = family.hrefs[role];
    if (href.empty()) continue;
    // Styles sharing a file share its bytes.
    const auto same = std::find(family.hrefs, family.hrefs + role, href);
    if (same != family.hrefs + role) continue;

    size_t size = 0;
    if (!epub_->getItemSize(href, &size) || size == 0 || size > MAX_FONT_BYTES) {
      LOG_ERR("BKF", "Skipping font %s (%u bytes)", href.c_str(), static_cast<unsigned>(size));
      continue;
    }
    // psramNewArray falls back to internal RAM; keep that from starving the reader.
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) < size + PSRAM_HEADROOM) {
      LOG_ERR("BKF", "No PSRAM for font %s (%u bytes)", href.c_str(), static_cast<unsigned>(size));
      continue;
    }
    auto bytes = makeUniqueNoThrow<FontBytes>();
    if (!bytes) {
      LOG_ERR("BKF", "OOM: font bytes holder");
      continue;
    }
    bytes->data = freeink::font::psramNewArray<uint8_t>(size);
    if (!bytes->data) {
      LOG_ERR("BKF", "OOM: font %s (%u bytes)", href.c_str(), static_cast<unsigned>(size));
      continue;
    }
    BufferPrint sink(bytes->data, size);
    if (!epub_->readItemContentsToStream(href, sink, 4096) || sink.used() != size) {
      LOG_ERR("BKF", "Failed to read font %s", href.c_str());
      continue;
    }
    bytes->size = size;
    family.bytes[role] = std::move(bytes);
  }
  return family.bytes[TtfEpdFont::Regular] != nullptr;
}

bool BookFontSystem::loadFont(Family& family, const uint8_t pointSize, const int fontId) {
  for (auto& loaded : loaded_) {
    if (loaded.fontId == fontId) {
      loaded.lastUse = ++clock_;
      return true;
    }
  }
  if (!readFamilyBytes(family)) return false;

  if (loaded_.size() >= MAX_LOADED) {
    const auto oldest = std::min_element(loaded_.begin(), loaded_.end(),
                                         [](const Loaded& a, const Loaded& b) { return a.lastUse < b.lastUse; });
    renderer_->unregisterTtfFont(oldest->fontId);
    renderer_->removeFont(oldest->fontId);
    loaded_.erase(oldest);
  }
  auto font = makeUniqueNoThrow<TtfEpdFont>();
  if (!font) {
    LOG_ERR("BKF", "OOM: TtfEpdFont @%upt", pointSize);
    return false;
  }
  for (int role = 0; role < 4; ++role) {
    if (family.hrefs[role].empty()) continue;
    // A role without its own bytes reuses the earlier role that read the same file.
    const FontBytes* bytes = family.bytes[role].get();
    for (int other = 0; !bytes && other < 4; ++other) {
      if (family.bytes[other] && family.hrefs[other] == family.hrefs[role]) bytes = family.bytes[other].get();
    }
    if (bytes) font->addResidentSource(static_cast<uint8_t>(role), bytes->data, static_cast<uint32_t>(bytes->size));
  }
  if (!font->load(pointSize, /*twoBit=*/true, /*glyphCacheBytes=*/64 * 1024, /*maxGlyphs=*/512)) {
    LOG_ERR("BKF", "Failed to load embedded font @%upt", pointSize);
    return false;
  }
  font->build(" ");
  renderer_->insertFont(fontId, font->family());
  renderer_->registerTtfFont(fontId, font.get());
  loaded_.push_back({fontId, ++clock_, std::move(font)});
  LOG_DBG("BKF", "Loaded embedded font %08x @%upt (heap free %u)", static_cast<unsigned>(family.hash), pointSize,
          static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

#endif  // CROSSPOINT_VECTOR_FONTS
