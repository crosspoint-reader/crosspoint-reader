#include "SdCardFontRegistry.h"

#if CROSSPOINT_VECTOR_FONTS
#include <FtFont.h>
#endif
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <strings.h>  // strcasecmp

#include <algorithm>
#include <cstdlib>
#include <cstring>

// One file entry per installed size is resident from boot; keep it to the
// existing two-pointer footprint, including scalar metadata in its padding.
static_assert(sizeof(SdCardFontFileInfo) == 2 * sizeof(void*), "SdCardFontFileInfo grew");

// --- SdCardFontFamilyInfo helpers ---

const SdCardFontFileInfo* SdCardFontFamilyInfo::findFile(uint8_t size, uint8_t style) const {
  for (const auto& f : files) {
    if (f.pointSize == size && f.style == style) return &f;
  }
  return nullptr;
}

const SdCardFontFileInfo* SdCardFontFamilyInfo::findNearestSize(const uint8_t pointSize, const uint8_t style) const {
  // The reader stores an actual point size, so an exact match is the norm and
  // falls out of the delta search below (delta 0). The search only matters when
  // the size was carried over from a family that ships different sizes; the
  // caller then persists the snapped size (SdCardFontSystem::ensureLoaded).
  const SdCardFontFileInfo* best = nullptr;
  uint8_t bestDelta = 255;
  for (const auto& f : files) {
    if (f.style != style) continue;
    const uint8_t delta = f.pointSize > pointSize ? f.pointSize - pointSize : pointSize - f.pointSize;
    // Ties resolve to the smaller size, matching snapToNearestPointSize().
    if (!best || delta < bestDelta || (delta == bestDelta && f.pointSize < best->pointSize)) {
      best = &f;
      bestDelta = delta;
    }
  }
  return best;
}

bool SdCardFontFamilyInfo::hasSize(uint8_t size) const {
  for (const auto& f : files) {
    if (f.pointSize == size) return true;
  }
  return false;
}

std::vector<uint8_t> SdCardFontFamilyInfo::availableSizes() const {
  std::vector<uint8_t> sizes;
  for (const auto& f : files) {
    bool found = false;
    for (uint8_t s : sizes) {
      if (s == f.pointSize) {
        found = true;
        break;
      }
    }
    if (!found) sizes.push_back(f.pointSize);
  }
  std::sort(sizes.begin(), sizes.end());
  return sizes;
}

namespace {
// "_<size>.cpfont": the part of a canonical v4 filename after the family name.
void cpfontTail(const uint8_t size, char* out, const size_t outSize) { snprintf(out, outSize, "_%u.cpfont", size); }
constexpr size_t CPFONT_TAIL_SIZE = sizeof("_255.cpfont");

// Heap copy of a filename for SdCardFontFileInfo::name; null when out of memory.
std::unique_ptr<char[]> copyFileName(const char* filename) {
  const size_t bytes = strlen(filename) + 1;
  auto copy = makeUniqueNoThrow<char[]>(bytes);
  if (!copy) {
    LOG_ERR("SDREG", "OOM: %zu byte filename %s", bytes, filename);
    return nullptr;
  }
  memcpy(copy.get(), filename, bytes);
  return copy;
}

// Loaders rebuild the path through filePath() into a PATH_BUFFER_SIZE buffer,
// so a file whose path does not fit can never be opened: drop it at discovery.
// `family` must already carry its name, root and loose flag.
bool pathFits(const SdCardFontFamilyInfo& family, const SdCardFontFileInfo& file) {
  char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
  return family.filePath(file, path, sizeof(path));
}
}  // namespace

bool SdCardFontFamilyInfo::filePath(const SdCardFontFileInfo& file, char* out, const size_t outSize) const {
  if (!root) return false;
  int n;
  if (!file.name) {
    char tail[CPFONT_TAIL_SIZE];
    cpfontTail(file.pointSize, tail, sizeof(tail));
    n = snprintf(out, outSize, "%s/%s/%s%s", root, name.c_str(), name.c_str(), tail);
  } else if (loose) {
    n = snprintf(out, outSize, "%s/%s", root, file.name.get());
  } else {
    n = snprintf(out, outSize, "%s/%s/%s", root, name.c_str(), file.name.get());
  }
  return n > 0 && static_cast<size_t>(n) < outSize;
}

// --- SdCardFontRegistry ---

bool SdCardFontRegistry::parseFilename(const char* filename, uint8_t& size, uint8_t& style) {
  // V4 naming: <name>_<size>.cpfont (e.g. Bookerly-SD_14.cpfont)
  // Use an ends-with check rather than strstr() so that in-progress downloads
  // like "Foo_14.cpfont.tmp" or backups like "Foo_14.cpfont~" aren't accepted.
  static constexpr char kExt[] = ".cpfont";
  static constexpr size_t kExtLen = sizeof(kExt) - 1;
  const size_t nameLen = strlen(filename);
  if (nameLen <= kExtLen) return false;
  if (strcmp(filename + nameLen - kExtLen, kExt) != 0) return false;
  const char* ext = filename + nameLen - kExtLen;

  size_t baseLen = ext - filename;
  if (baseLen == 0 || baseLen > 127) return false;

  char base[128];
  memcpy(base, filename, baseLen);
  base[baseLen] = '\0';

  char* lastUnderscore = strrchr(base, '_');
  if (!lastUnderscore || lastUnderscore == base) return false;

  const char* sizeStr = lastUnderscore + 1;
  char* endPtr;
  long sizeVal = strtol(sizeStr, &endPtr, 10);
  if (endPtr == sizeStr || *endPtr != '\0' || sizeVal < 1 || sizeVal > 255) return false;
  size = static_cast<uint8_t>(sizeVal);
  // V4 .cpfont files bundle every style (regular/bold/italic/bold-italic) into
  // one file, so style is always 0 at the registry level. The per-style
  // bitstream is selected later by SdCardFont::getEpdFont(style). The `style`
  // field in SdCardFontFileInfo is reserved for future formats that split
  // styles across files; scanDirectory() defends against accidental
  // (pointSize, style) collisions in that scenario.
  style = 0;
  return true;
}

bool SdCardFontRegistry::isCanonicalCpfontName(const char* filename, const std::string& family, const uint8_t size) {
  if (strncmp(filename, family.c_str(), family.size()) != 0) return false;
  char tail[CPFONT_TAIL_SIZE];
  cpfontTail(size, tail, sizeof(tail));
  return strcmp(filename + family.size(), tail) == 0;
}

#if CROSSPOINT_VECTOR_FONTS

namespace {
enum VectorStyleBits : uint8_t {
  HINT_BOLD = 1 << 0,
  HINT_ITALIC = 1 << 1,
  ROLE_REGULAR = 1 << 2,
  ROLE_BOLD = 1 << 3,
  ROLE_ITALIC = 1 << 4,
  ROLE_BOLD_ITALIC = 1 << 5,
  ROLE_MASK = ROLE_REGULAR | ROLE_BOLD | ROLE_ITALIC | ROLE_BOLD_ITALIC,
};
enum class VectorRole : uint8_t { Regular, Bold, Italic, BoldItalic, Count };
constexpr size_t VECTOR_STYLE_COUNT = static_cast<size_t>(VectorRole::Count);
constexpr uint8_t VECTOR_ROLE_HINTS[] = {0, HINT_BOLD, HINT_ITALIC, HINT_BOLD | HINT_ITALIC};
bool isIntermediateWeight(const uint16_t weight) { return weight > 400 && weight < 700; }
bool isStyleSeparator(const char c) { return c == ' ' || c == '-' || c == '_'; }
}  // namespace

bool SdCardFontRegistry::parseVectorFontName(const char* filename, size_t& baseLen) {
  static constexpr const char* kExts[] = {".ttf", ".otf", ".ttc"};
  const size_t nameLen = strlen(filename);
  for (const char* ext : kExts) {
    const size_t extLen = strlen(ext);
    if (nameLen <= extLen) continue;
    const char* tail = filename + nameLen - extLen;
    if (strcasecmp(tail, ext) == 0) {
      baseLen = nameLen - extLen;
      return baseLen > 0 && baseLen <= 127;
    }
  }
  return false;
}

uint8_t SdCardFontRegistry::guessVectorStyle(const char* baseName, const size_t baseLen) {
  uint8_t guess = 0;
  for (size_t i = 0; i < baseLen; ++i) {
    if (baseLen - i >= 4 && strncasecmp(baseName + i, "bold", 4) == 0) guess |= HINT_BOLD;
    if (baseLen - i >= 6 && strncasecmp(baseName + i, "italic", 6) == 0) guess |= HINT_ITALIC;
    if (baseLen - i >= 7 && strncasecmp(baseName + i, "oblique", 7) == 0) guess |= HINT_ITALIC;
  }

  static constexpr struct {
    const char* name;
    uint8_t role;
    uint8_t joinAt;  // Optional separator within a combined suffix.
  } suffixes[] = {{"BoldItalic", ROLE_BOLD_ITALIC, 4},
                  {"BoldOblique", ROLE_BOLD_ITALIC, 4},
                  {"BlackOblique", ROLE_BOLD_ITALIC, 5},
                  {"BlackItalic", ROLE_BOLD_ITALIC, 5},
                  {"Regular", ROLE_REGULAR, 0},
                  {"Book", ROLE_REGULAR, 0},
                  {"Roman", ROLE_REGULAR, 0},
                  {"Bold", ROLE_BOLD, 0},
                  {"Black", ROLE_BOLD, 0},
                  {"Italic", ROLE_ITALIC, 0},
                  {"Oblique", ROLE_ITALIC, 0}};
  // Combined styles precede their italic-only tails. baseLen excludes the extension.
  for (const auto& suffix : suffixes) {
    const size_t len = strlen(suffix.name);
    if (len < baseLen && isStyleSeparator(baseName[baseLen - len - 1]) &&
        strncasecmp(baseName + baseLen - len, suffix.name, len) == 0) {
      return guess | suffix.role;
    }
    if (!suffix.joinAt || len + 1 >= baseLen) continue;
    const size_t start = baseLen - len - 1;
    // Match one separator inside a combined suffix, e.g. Family-Bold-Italic.ttf.
    if (isStyleSeparator(baseName[start - 1]) && isStyleSeparator(baseName[start + suffix.joinAt]) &&
        strncasecmp(baseName + start, suffix.name, suffix.joinAt) == 0 &&
        strncasecmp(baseName + start + suffix.joinAt + 1, suffix.name + suffix.joinAt, len - suffix.joinAt) == 0) {
      return guess | suffix.role;
    }
  }
  return guess;
}

// FtFont::ReadFn over a HalFile (absolute-offset reads; count 0 is a seek probe).
unsigned long SdCardFontRegistry::halFileRead(void* ctx, const unsigned long offset, unsigned char* buffer,
                                              const unsigned long count) {
  auto* f = static_cast<HalFile*>(ctx);
  if (f == nullptr || !*f) return 0;
  if (!f->seek(static_cast<size_t>(offset))) return 0;
  if (count == 0) return 0;
  const int n = f->read(buffer, count);
  return n < 0 ? 0 : static_cast<unsigned long>(n);
}

void SdCardFontRegistry::refineVectorStyles(const char* dirPath, std::vector<SdCardFontFileInfo>& files) {
  static constexpr int WGHT_REGULAR = 400;
  static constexpr int WGHT_DIST_BOLD = 300;

  // - Validate every candidate with the existing face inspector; discard any failure.
  // - Prefer explicit suffix roles over filename hints OR metadata.
  // - Record metadata weight; OR its bold (>=700) and italic flags with filename hints.
  // - Rank weights nearest 400 for non-bold roles, regular weight + 300 for bold roles.
  //   Unsuffixed italic candidates instead target the selected regular weight.
  //   Equal distances prefer the heavier weight, then lexical filename order.
  //   Intermediate weights (401-699) can fill either weight role.
  // - Pick regular first, promoting italic, bold, or bold-italic if needed (in that order).
  //   Fill other roles from unused files, matching italics; never reuse a file.
  using freeink::font::FtFont;
  const auto invalid = [&](SdCardFontFileInfo& file) {
    char path[SdCardFontFamilyInfo::PATH_BUFFER_SIZE];
    snprintf(path, sizeof(path), "%s/%s", dirPath, file.name.get());
    HalFile f = Storage.open(path);
    FtFont::FaceInfo face;
    if (!f || f.isDirectory() ||
        FtFont::inspectStream(&halFileRead, &f, static_cast<unsigned long>(f.size()), face) !=
            FtFont::InspectResult::Ok) {
      LOG_ERR("SDREG", "Skipping unavailable or unsupported font: %s", path);
      return true;
    }
    file.weight = face.weight;
    if (face.weight >= 700) file.style |= HINT_BOLD;
    if (face.italic) file.style |= HINT_ITALIC;
    return false;
  };
  files.erase(std::remove_if(files.begin(), files.end(), invalid), files.end());
  if (files.empty()) return;
  const size_t missing = files.size();
  size_t picks[VECTOR_STYLE_COUNT] = {missing, missing, missing, missing};
  const auto pickUnused = [&](const size_t role) {
    size_t explicitPick = missing;
    size_t fallbackPick = missing;
    const bool wantBold = VECTOR_ROLE_HINTS[role] & HINT_BOLD;
    const bool wantItalic = VECTOR_ROLE_HINTS[role] & HINT_ITALIC;
    const int regularWeight = picks[0] == missing ? WGHT_REGULAR : static_cast<int>(files[picks[0]].weight);
    const int target = wantBold ? regularWeight + WGHT_DIST_BOLD : WGHT_REGULAR;
    const int fallbackTarget = wantItalic && !wantBold ? regularWeight : target;
    const auto better = [&](const size_t candidate, const size_t current, const int preferredWeight) {
      if (current == missing) return true;
      const int distance = std::abs(static_cast<int>(files[candidate].weight) - preferredWeight);
      const int bestDistance = std::abs(static_cast<int>(files[current].weight) - preferredWeight);
      if (distance != bestDistance) return distance < bestDistance;
      if (files[candidate].weight != files[current].weight) return files[candidate].weight > files[current].weight;
      return strcmp(files[candidate].name.get(), files[current].name.get()) < 0;
    };
    for (size_t i = 0; i < files.size(); ++i) {
      if (std::find(picks, picks + VECTOR_STYLE_COUNT, i) != picks + VECTOR_STYLE_COUNT) continue;
      if (files[i].style & ROLE_MASK) {
        if ((files[i].style & (ROLE_REGULAR << role)) && better(i, explicitPick, target)) explicitPick = i;
        continue;
      }
      const bool intermediate = isIntermediateWeight(files[i].weight);
      const bool bold = files[i].style & HINT_BOLD;
      const bool italic = files[i].style & HINT_ITALIC;
      if (italic != wantItalic || (!intermediate && bold != wantBold)) continue;
      if (better(i, fallbackPick, fallbackTarget)) fallbackPick = i;
    }
    return explicitPick != missing ? explicitPick : fallbackPick;
  };

  picks[0] = pickUnused(static_cast<size_t>(VectorRole::Regular));
  if (picks[0] == missing) {
    static constexpr VectorRole promotionOrder[] = {VectorRole::Italic, VectorRole::Bold, VectorRole::BoldItalic};
    for (const auto promotedRole : promotionOrder) {
      const size_t role = static_cast<size_t>(promotedRole);
      picks[0] = pickUnused(role);
      if (picks[0] == missing) continue;
      break;
    }
  }

  for (size_t role = 1; role < VECTOR_STYLE_COUNT; ++role) {
    picks[role] = pickUnused(role);
  }

  std::vector<SdCardFontFileInfo> selected;
  selected.reserve(std::min(VECTOR_STYLE_COUNT, files.size()));
  [[maybe_unused]] static constexpr const char* roleNames[] = {"Regular", "Bold", "Italic", "BoldItalic"};
  for (size_t role = 0; role < VECTOR_STYLE_COUNT; ++role) {
    LOG_DBG("SDREG", "%s: %s -> %s", dirPath, roleNames[role],
            picks[role] == missing ? "(not supplied)" : files[picks[role]].name.get());
    if (picks[role] == missing) continue;
    files[picks[role]].style = static_cast<uint8_t>(role);
    selected.push_back(std::move(files[picks[role]]));
  }
  if (selected.size() < files.size()) {
    LOG_DBG("SDREG", "%s: %u of %u faces selected by style", dirPath, static_cast<unsigned>(selected.size()),
            static_cast<unsigned>(files.size()));
  }
  files = std::move(selected);
}

#endif  // CROSSPOINT_VECTOR_FONTS

void SdCardFontRegistry::scanDirectory(const char* dirPath, SdCardFontFamilyInfo& family) {
  HalFile dir = Storage.open(dirPath);
  if (!dir || !dir.isDirectory()) return;

  // Collect .cpfont and vector (.ttf/.otf/.ttc) candidates separately in one
  // pass (the dir handle is forward-only), then commit whichever kind the folder
  // holds. .cpfont wins if a folder somehow contains both, since a pre-rasterized
  // bitmap family is the more specific artifact.
  std::vector<SdCardFontFileInfo> cpfontFiles;
  std::vector<SdCardFontFileInfo> vectorFiles;

  char nameBuffer[128];
  while (true) {
    HalFile entry = dir.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.close();
      continue;
    }

    entry.getName(nameBuffer, sizeof(nameBuffer));
    entry.close();

    // Skip macOS resource fork files (._*) and other hidden files
    if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

    uint8_t size, style;
    if (parseFilename(nameBuffer, size, style)) {
      // .cpfont: reject duplicate (pointSize, style) — style is always 0 in v4,
      // so two files at the same size would silently shadow each other.
      bool duplicate = false;
      for (const auto& existing : cpfontFiles) {
        if (existing.pointSize == size && existing.style == style) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) {
        LOG_ERR("SDREG", "Duplicate font %s in %s — skipping", nameBuffer, dirPath);
        continue;
      }
      SdCardFontFileInfo info;
      if (!isCanonicalCpfontName(nameBuffer, family.name, size)) {
        info.name = copyFileName(nameBuffer);
        if (!info.name) continue;
      }
      info.pointSize = size;
      info.style = style;
      if (!pathFits(family, info)) {
        LOG_ERR("SDREG", "Path too long for %s in %s — skipping", nameBuffer, dirPath);
        continue;
      }
      cpfontFiles.push_back(std::move(info));
      continue;
    }

#if CROSSPOINT_VECTOR_FONTS
    size_t baseLen = 0;
    if (parseVectorFontName(nameBuffer, baseLen)) {
      // Keep filename hints and explicit suffix roles until refineVectorStyles
      // resolves them to one file per role.
      SdCardFontFileInfo info;
      info.name = copyFileName(nameBuffer);
      if (!info.name) continue;
      info.pointSize = 0;  // size-free
      info.style = guessVectorStyle(nameBuffer, baseLen);
      if (!pathFits(family, info)) {
        LOG_ERR("SDREG", "Path too long for %s in %s — skipping", nameBuffer, dirPath);
        continue;
      }
      vectorFiles.push_back(std::move(info));
    }
#endif
  }

  if (!cpfontFiles.empty()) {
    family.vector = false;
    family.files = std::move(cpfontFiles);
  }
#if CROSSPOINT_VECTOR_FONTS
  else if (!vectorFiles.empty()) {
    refineVectorStyles(dirPath, vectorFiles);
    family.vector = true;
    family.files = std::move(vectorFiles);
  }
#endif
}

// Scan a single root (e.g. "/.fonts") and append its families to `out`.
// Skips families whose names already exist in `out` (de-duplicates between
// the hidden and visible roots — first scan wins).
void SdCardFontRegistry::scanRoot(const char* rootPath, std::vector<SdCardFontFamilyInfo>& out) {
  HalFile root = Storage.open(rootPath);
  if (!root) {
    LOG_DBG("SDREG", "Fonts directory not found: %s", rootPath);
    return;
  }
  if (!root.isDirectory()) {
    LOG_ERR("SDREG", "Fonts path is not a directory: %s", rootPath);
    return;
  }

  char nameBuffer[128];
  while (true) {
    HalFile entry = root.openNextFile();
    if (!entry) break;
    if (entry.isDirectory()) {
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();

      // Skip hidden/system directories inside the root (macOS ._*, .Trashes, etc.)
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;

      // De-dup by family name across roots.
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == nameBuffer) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = nameBuffer;
      family.root = rootPath;
      std::string subDirPath = std::string(rootPath) + "/" + nameBuffer;
      SdCardFontRegistry::scanDirectory(subDirPath.c_str(), family);

      if (!family.files.empty()) {
        out.push_back(std::move(family));
        LOG_DBG("SDREG", "Found family: %s (%d files) in %s", out.back().name.c_str(),
                static_cast<int>(out.back().files.size()), rootPath);
      }
    } else {
#if CROSSPOINT_VECTOR_FONTS
      // Loose TrueType/OpenType file directly under the root (e.g.
      // /fonts/Bookerly.ttf). Rendered at any size via the FreeInkFont engine.
      entry.getName(nameBuffer, sizeof(nameBuffer));
      entry.close();
      if (nameBuffer[0] == '.' || nameBuffer[0] == '_') continue;
      size_t baseLen = 0;
      if (!parseVectorFontName(nameBuffer, baseLen)) continue;

      std::string famName(nameBuffer, baseLen);  // filename without extension
      bool exists = false;
      for (const auto& fam : out) {
        if (fam.name == famName) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      SdCardFontFamilyInfo family;
      family.name = famName;
      family.root = rootPath;
      family.vector = true;
      family.loose = true;
      SdCardFontFileInfo info;
      info.name = copyFileName(nameBuffer);
      if (!info.name) continue;
      info.pointSize = 0;  // size-free
      info.style = 0;
      if (!pathFits(family, info)) {
        LOG_ERR("SDREG", "Path too long for %s in %s — skipping", nameBuffer, rootPath);
        continue;
      }
      family.files.push_back(std::move(info));
      refineVectorStyles(rootPath, family.files);
      if (family.files.empty()) continue;
      out.push_back(std::move(family));
      LOG_DBG("SDREG", "Found vector font: %s in %s", famName.c_str(), rootPath);
#endif  // CROSSPOINT_VECTOR_FONTS — loose .ttf/.otf files are ignored without the engine
    }
  }
}

bool SdCardFontRegistry::discover() {
  families_.clear();

  // Hidden root is scanned first so it wins on name collisions, matching the
  // sleep-folder pattern (/.sleep preferred over /sleep).
  scanRoot(FONTS_DIR_HIDDEN, families_);
  scanRoot(FONTS_DIR_VISIBLE, families_);

  // Sort families alphabetically
  std::sort(families_.begin(), families_.end(),
            [](const SdCardFontFamilyInfo& a, const SdCardFontFamilyInfo& b) { return a.name < b.name; });

  // Cap at MAX_SD_FAMILIES
  if (static_cast<int>(families_.size()) > MAX_SD_FAMILIES) {
    families_.resize(MAX_SD_FAMILIES);
  }
  shrinkToFit(families_);  // the list is read-only until the next scan

  LOG_DBG("SDREG", "Discovery complete: %d families", static_cast<int>(families_.size()));
  return !families_.empty();
}

const char* SdCardFontRegistry::findFamilyRoot(const char* familyName) {
  if (!familyName || !*familyName) return nullptr;
  char path[160];
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_HIDDEN, familyName);
  if (Storage.exists(path)) return FONTS_DIR_HIDDEN;
  snprintf(path, sizeof(path), "%s/%s", FONTS_DIR_VISIBLE, familyName);
  if (Storage.exists(path)) return FONTS_DIR_VISIBLE;
  return nullptr;
}

const char* SdCardFontRegistry::defaultWriteRoot() {
  // If exactly one of the roots already exists, keep using it. Otherwise
  // (neither exists, or both exist) prefer the hidden root for new installs.
  bool hiddenExists = Storage.exists(FONTS_DIR_HIDDEN);
  bool visibleExists = Storage.exists(FONTS_DIR_VISIBLE);
  if (hiddenExists) return FONTS_DIR_HIDDEN;
  if (visibleExists) return FONTS_DIR_VISIBLE;
  return FONTS_DIR_HIDDEN;
}

const SdCardFontFamilyInfo* SdCardFontRegistry::findFamily(const std::string& name) const {
  for (const auto& f : families_) {
    if (f.name == name) return &f;
  }
  return nullptr;
}

int SdCardFontRegistry::getFamilyIndex(const std::string& name) const {
  for (int i = 0; i < static_cast<int>(families_.size()); i++) {
    if (families_[i].name == name) return i;
  }
  return -1;
}
