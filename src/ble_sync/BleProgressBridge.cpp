#include "BleProgressBridge.h"

#include <Epub.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Logging.h>
#include <MD5Builder.h>

#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>

#include "CrossPointState.h"                    // APP_STATE.openEpubPath
#include "KOReaderCredentialStore.h"            // KOREADER_STORE match method
#include "KOReaderDocumentId.h"                 // document hash
#include "ProgressMapper.h"                     // toSavedProgress / toCrossPoint
#include "RecentBooksStore.h"                   // RECENT_BOOKS — the v2 book set
#include "activities/reader/EpubReaderUtils.h"  // saveProgress

namespace {
constexpr char kDeviceName[] = "CrossPoint";
constexpr char kCacheDir[] = "/.crosspoint";

// Cache dir for a book path — MUST match Epub's ctor derivation exactly
// (Epub.cpp: cacheDir + "/epub_" + hash(filepath)). Lets us read a book's
// progress-time.bin without loading the epub.
std::string cachePathForBook(const std::string& path) {
  return std::string(kCacheDir) + "/epub_" + std::to_string(std::hash<std::string>{}(path));
}

std::string docHash(const std::string& path) {
  if (path.empty()) return "";
  if (KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME) {
    return KOReaderDocumentId::calculateFromFilename(path);
  }
  return KOReaderDocumentId::calculate(path);
}

// Byte-simple, deterministic normalization (must match the phone exactly — see
// PROTOCOL-v1.md §2): lowercase ASCII A-Z only, trim, collapse internal
// whitespace to a single space. Non-ASCII bytes pass through unchanged.
std::string normForHash(const std::string& s) {
  std::string out;
  out.reserve(s.size());
  bool prevSpace = true;  // trims leading + collapses
  for (char ch : s) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
      if (!prevSpace) {
        out.push_back(' ');
        prevSpace = true;
      }
    } else {
      out.push_back((c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : ch);
      prevSpace = false;
    }
  }
  if (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

std::string md5Hex(const std::string& s) {
  MD5Builder md5;
  md5.begin();
  md5.add(reinterpret_cast<uint8_t*>(const_cast<char*>(s.data())), s.size());
  md5.calculate();
  return std::string(md5.toString().c_str());
}

// title_hash = MD5( norm(title) 0x1F norm(author) ).
std::string titleHashFromMeta(const std::string& title, const std::string& author) {
  std::string joined = normForHash(title);
  joined.push_back('\x1F');
  joined += normForHash(author);
  return md5Hex(joined);
}

std::string titleHashOf(const std::shared_ptr<Epub>& epub) {
  return titleHashFromMeta(epub->getTitle(), epub->getAuthor());
}

// Mirrors KOReaderSyncActivity::ensureEpubLoaded (metadata-only load).
std::shared_ptr<Epub> loadEpub(const std::string& path) {
  auto epub = std::make_shared<Epub>(path, "/.crosspoint");
  epub->setupCacheDir();
  if (!epub->load(/*buildIfMissing=*/false, /*skipLoadingCss=*/true)) {
    LOG_ERR("BleProg", "epub load failed: %s", path.c_str());
    return nullptr;
  }
  return epub;
}

// Decode the 6-byte little-endian progress.bin (spine, page, count). 4 bytes ok.
bool readProgressBin(const std::string& cachePath, int& spine, int& page, int& count) {
  const std::string file = cachePath + "/progress.bin";
  HalFile f;
  if (!Storage.openFileForRead("BleProg", file, f)) return false;
  uint8_t d[6] = {0};
  const size_t n = f.read(d, sizeof(d));
  if (n < 4) return false;
  spine = d[0] | (d[1] << 8);
  page = d[2] | (d[3] << 8);
  count = (n >= 6) ? (d[4] | (d[5] << 8)) : 0;
  return true;
}

// Per-book last-save time (unix s) stamped by EpubReaderUtils::saveProgress. 0 = none.
int64_t readProgressTime(const std::string& cachePath) {
  HalFile f;
  if (!Storage.openFileForRead("BleProg", cachePath + "/progress-time.bin", f)) return 0;
  uint8_t d[8] = {0};
  if (f.read(d, sizeof(d)) < 8) return 0;
  int64_t t = 0;
  for (int i = 0; i < 8; i++) t |= (static_cast<int64_t>(d[i]) << (8 * i));
  return t;
}

void writeProgressTime(const std::string& cachePath, int64_t t) {
  uint8_t d[8];
  for (int i = 0; i < 8; i++) d[i] = static_cast<uint8_t>((static_cast<uint64_t>(t) >> (8 * i)) & 0xFF);
  HalFile f;
  if (Storage.openFileForWrite("BleProg", cachePath + "/progress-time.bin", f)) {
    f.write(d, sizeof(d));
    f.flush();
  }
}
}  // namespace

std::string BleProgress::currentDocumentHash() { return docHash(APP_STATE.openEpubPath); }

std::string BleProgress::currentTitleHash() {
  const std::string path = APP_STATE.openEpubPath;
  if (path.empty()) return "";
  auto epub = loadEpub(path);
  return epub ? titleHashOf(epub) : "";
}

bool BleProgress::getForPath(const std::string& path, KOReaderProgress& out, std::string& titleHash,
                             const std::string& deviceId) {
  if (path.empty()) {
    LOG_DBG("BleProg", "getForPath: empty path");
    return false;
  }
  const std::string hash = docHash(path);
  if (hash.empty()) return false;

  auto epub = loadEpub(path);
  if (!epub) return false;

  int spine = 0, page = 0, count = 0;
  if (!readProgressBin(epub->getCachePath(), spine, page, count)) {
    LOG_DBG("BleProg", "no progress.bin (unread book)");
    return false;
  }

  CrossPointPosition pos{};
  pos.spineIndex = spine;
  pos.pageNumber = page;
  pos.totalPages = count;
  const SavedProgressPosition sp = ProgressMapper::toSavedProgress(epub, pos);

  out.document = hash;
  out.progress = sp.xpath;
  out.percentage = sp.percentage;
  out.device = kDeviceName;
  out.deviceId = deviceId;
  out.timestamp = readProgressTime(epub->getCachePath());  // real last-save time (0 = unclocked)
  titleHash = titleHashOf(epub);
  LOG_DBG("BleProg", "local %s %.1f%% title=%s", hash.c_str(), sp.percentage * 100, titleHash.c_str());
  return true;
}

bool BleProgress::getLocal(KOReaderProgress& out, std::string& titleHash, const std::string& deviceId) {
  return getForPath(APP_STATE.openEpubPath, out, titleHash, deviceId);
}

std::vector<BleSyncProtocol::ManifestEntry> BleProgress::buildLocalManifest(size_t maxN) {
  std::vector<BleSyncProtocol::ManifestEntry> books;
  const auto& recent = RECENT_BOOKS.getBooks();  // most-recent-first
  for (const auto& b : recent) {
    if (books.size() >= maxN) break;
    if (b.title.empty()) continue;  // can't build a stable title_hash without a title
    BleSyncProtocol::ManifestEntry e;
    e.titleHash = titleHashFromMeta(b.title, b.author);
    e.updatedAt = readProgressTime(cachePathForBook(b.path));  // 0 = have book, no clocked progress
    books.push_back(std::move(e));
  }
  LOG_DBG("BleProg", "manifest: %d books", (int)books.size());
  return books;
}

std::vector<std::pair<std::string, std::string>> BleProgress::pathTitleHashes(size_t maxN) {
  std::vector<std::pair<std::string, std::string>> out;
  const auto& recent = RECENT_BOOKS.getBooks();
  for (const auto& b : recent) {
    if (out.size() >= maxN) break;
    if (b.title.empty() || b.path.empty()) continue;
    out.emplace_back(b.path, titleHashFromMeta(b.title, b.author));
  }
  return out;
}

std::string BleProgress::pathForHash(const std::string& document, const std::string& titleHash) {
  const auto& recent = RECENT_BOOKS.getBooks();
  // Prefer the byte-independent title_hash match (survives X4-optimized copies).
  if (!titleHash.empty()) {
    for (const auto& b : recent) {
      if (b.title.empty()) continue;
      if (titleHashFromMeta(b.title, b.author) == titleHash) return b.path;
    }
  }
  // Fall back to the document hash (reads file bytes for BINARY — do it last).
  if (!document.empty()) {
    for (const auto& b : recent) {
      if (docHash(b.path) == document) return b.path;
    }
  }
  return "";
}

int64_t BleProgress::localTimeForHash(const std::string& titleHash) {
  const std::string path = pathForHash("", titleHash);
  if (path.empty()) return 0;
  return readProgressTime(cachePathForBook(path));
}

size_t BleProgress::backfillUnclockedTimestamps(int64_t now) {
  if (now <= 1000000000) return 0;
  size_t fixed = 0;
  const auto& recent = RECENT_BOOKS.getBooks();
  for (const auto& b : recent) {
    const std::string cachePath = cachePathForBook(b.path);
    // Only books with an EXISTING stamp file that reads 0 — the explicit
    // "changed while unclocked" marker. A missing file means the book was
    // never read with sync on; stamping it now would wrongly win
    // newest-wins against the phone.
    HalFile probe;
    if (!Storage.openFileForRead("BleProg", cachePath + "/progress-time.bin", probe)) continue;
    uint8_t d[8] = {0};
    const bool isZero = (probe.read(d, sizeof(d)) == 8) && !(d[0] | d[1] | d[2] | d[3] | d[4] | d[5] | d[6] | d[7]);
    if (!isZero) continue;
    writeProgressTime(cachePath, now);
    fixed++;
  }
  if (fixed > 0) {
    LOG_DBG("BleProg", "backfilled %d unclocked progress stamps", (int)fixed);
  }
  return fixed;
}

bool BleProgress::applyRemote(const KOReaderProgress& in, const std::string& remoteTitleHash, GfxRenderer& renderer) {
  // Resolve the target book across RECENT_BOOKS (v2: not just the open book).
  // Prefer the currently/last-open book when it matches (cheapest, no scan).
  std::string path;
  const std::string openPath = APP_STATE.openEpubPath;
  if (!openPath.empty()) {
    if (auto openEpub = loadEpub(openPath)) {
      const bool openMatch =
          (docHash(openPath) == in.document) || (!remoteTitleHash.empty() && remoteTitleHash == titleHashOf(openEpub));
      if (openMatch) path = openPath;
    }
  }
  if (path.empty()) path = pathForHash(in.document, remoteTitleHash);
  if (path.empty()) {
    LOG_DBG("BleProg", "no recent book matches doc=%s title=%s", in.document.c_str(), remoteTitleHash.c_str());
    return false;
  }

  auto epub = loadEpub(path);
  if (!epub) return false;

  // Newest-wins: only overwrite the local position if the remote is strictly
  // newer and carries a real clock. Local time 0 = unclocked/unread -> accept.
  const int64_t localTime = readProgressTime(epub->getCachePath());
  if (!(in.timestamp > 0 && in.timestamp > localTime)) {
    LOG_DBG("BleProg", "not newer: remote=%lld local=%lld -> keep local", (long long)in.timestamp,
            (long long)localTime);
    return false;
  }

  // No active-reading grace: BLE applies only run at boot / book-open / book-
  // exit (never while the reader is the live screen — event-2 is gone), and
  // strict newest-wins above already keeps a genuinely-newer local read from
  // being overwritten. The 90s grace only made sync feel dead during testing.

  const SavedProgressPosition sp{in.progress, in.percentage};
  const CrossPointPosition cp = ProgressMapper::toCrossPoint(epub, sp, renderer);
  if (!EpubReaderUtils::saveProgress(*epub, cp.spineIndex, cp.pageNumber, cp.totalPages)) {
    LOG_ERR("BleProg", "saveProgress failed");
    return false;
  }
  // The applied position carries the REMOTE's timestamp (saveProgress stamped
  // 'now'; overwrite so a later reconcile sees the true source time).
  writeProgressTime(epub->getCachePath(), in.timestamp);
  // Only a reader already holding this book in RAM needs a reload marker. Other
  // books will read their newly written progress.bin normally when opened. This
  // prevents later updates for unrelated books from overwriting the active
  // book's marker without adding a heap-backed per-book queue on the C3.
  if (APP_STATE.openEpubPath == path) APP_STATE.bleAppliedPath = path;
  LOG_DBG("BleProg", "applied %.1f%% -> spine=%d page=%d", in.percentage * 100, cp.spineIndex, cp.pageNumber);
  return true;
}
