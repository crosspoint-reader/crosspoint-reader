#if CROSSPOINT_DESSEN_APPS
#include "CrosspointServices.h"

#include <Bitmap.h>
#include <BookKey.h>
#include <Crypto.h>  // freeink::content::Crypto, RsaKeyPairDer
#include <DeviceSecret.h>
#include <Epub.h>
#include <FreeInkUIGfxRenderer.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalFrontlight.h>  // declares the `Frontlight` singleton
#include <HalGPIO.h>        // declares the `gpio` singleton (USB/charge detection)
#include <HalHaptics.h>
#include <HalPowerManager.h>  // declares the `powerManager` singleton
#include <HalStorage.h>
#include <I18n.h>  // I18N, Language, LANGUAGE_CODES
#include <LibraryBuilder.h>
#include <LibraryIndexFile.h>
#include <LibraryText.h>
#include <Logging.h>
#include <Memory.h>
#include <Utf8.h>
#include <WiFi.h>
#include <WifiCredentialStore.h>
#include <WolfsslCrypto.h>  // freeink::content::WolfsslCrypto
#include <sdk/dessen_abi.h>
#include <strings.h>
#include <time.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string_view>

#include "MappedInputManager.h"
#include "RecentBooksStore.h"
#include "components/UITheme.h"
#include "components/icons/cover.h"  // CoverIcon, the fallback-cover glyph
#include "network/HttpDownloader.h"
#include "util/BookProgress.h"

using freeink::content::Crypto;
using freeink::content::RsaKeyPairDer;
using freeink::content::WolfsslCrypto;

namespace {
// Preserve the guest buffer on failure and stop staging at its capacity.
class BoundedOutput {
 public:
  explicit BoundedOutput(size_t limit) : limit(limit) {}

  bool append(const void* src, size_t len) {
    if (!len) return true;
    if (len > limit - used) {
      error = DSN_ERR_TOO_BIG;
      return false;
    }
    const size_t needed = used + len;
    if (needed > capacity) {
      const size_t nextCapacity = std::max(needed, std::min(limit, std::max<size_t>(256, capacity * 2)));
      auto next = makeUniqueNoThrow<uint8_t[]>(nextCapacity);
      if (!next) {
        LOG_ERR("DSN", "OOM: %zu bytes for syscall output", nextCapacity);
        error = DSN_ERR_IO;
        return false;
      }
      if (used) memcpy(next.get(), data.get(), used);
      data = std::move(next);
      capacity = nextCapacity;
    }
    memcpy(data.get() + used, src, len);
    used = needed;
    return true;
  }

  int copyTo(void* out) const {
    if (used) memcpy(out, data.get(), used);
    return static_cast<int>(used);
  }

  int error = 0;

 private:
  const size_t limit;
  size_t capacity = 0;
  size_t used = 0;
  std::unique_ptr<uint8_t[]> data;
};

bool shelfControl(unsigned char c) { return c < 0x20 || c == 0x7f; }

bool shelfPathValid(std::string_view path) {
  return !path.empty() && path.front() == '/' && path.size() <= DSN_BOOK_PATH_MAX &&
         std::none_of(path.begin(), path.end(), shelfControl);
}

int writeShelfRow(char* out, uint32_t cap, std::string_view path, std::string_view title, std::string_view author) {
  if (!shelfPathValid(path)) {
    if (!cap) return DSN_ERR_TOO_BIG;
    *out = '\n';
    return 1;
  }
  constexpr size_t TEXT_MAX = 200;  // Size accepted by Dessen's text syscall.
  if (title.size() > TEXT_MAX) title = title.substr(0, utf8SafeTruncateBuffer(title.data(), TEXT_MAX));
  if (author.size() > TEXT_MAX) author = author.substr(0, utf8SafeTruncateBuffer(author.data(), TEXT_MAX));
  constexpr std::string_view suffix = "\t-1\n";
  const size_t bytes = path.size() + title.size() + author.size() + 2 + suffix.size();
  if (bytes > cap) return DSN_ERR_TOO_BIG;

  char* pos = out;
  memcpy(pos, path.data(), path.size());
  pos += path.size();
  for (std::string_view field : {title, author}) {
    *pos++ = '\t';
    for (char c : field) *pos++ = shelfControl(c) ? ' ' : c;
  }
  memcpy(pos, suffix.data(), suffix.size());
  return static_cast<int>(bytes);
}

// PKCS#7: pad to a 16-byte boundary, or validate+strip. Returns padded length
// (encrypt) / plaintext length (decrypt), or -1 on bad padding.
int pkcs7Strip(const uint8_t* buf, uint32_t len) {
  if (len == 0 || (len & 15)) return -1;
  const uint8_t pad = buf[len - 1];
  if (pad < 1 || pad > 16 || pad > len) return -1;
  for (uint32_t i = 0; i < pad; i++)
    if (buf[len - 1 - i] != pad) return -1;
  return static_cast<int>(len - pad);
}
}  // namespace

// Cache the native Recent tab's overlap lookup for this launcher's lifetime.
struct CrosspointServices::ShelfOverlap {
  static constexpr size_t MAX_OPENED = RecentBooksStore::MAX_RECENT_BOOKS;
  uint64_t paths[MAX_OPENED] = {};
  uint16_t rows[MAX_OPENED] = {};
  uint16_t openedCount = 0, bookCount = 0, nextFirstSeen = 0, hiddenCount = 0;
  uint32_t indexSize = 0;

  bool resolve(library::LibraryIndexFile& index, const library::BookIdentity* identities, uint16_t count) {
    const auto& header = index.header();
    if (header.selfSize == indexSize && header.bookCount == bookCount && header.nextFirstSeen == nextFirstSeen &&
        count == openedCount && std::equal(identities, identities + count, paths, [](const auto& book, uint64_t hash) {
          return book.pathHash == hash;
        }))
      return true;

    uint16_t ascending[MAX_OPENED];
    if (!index.recentRowsFor(identities, count, ascending)) return false;

    // An opened file can have changed size since the current index was written.
    library::BookIdentity unresolved[MAX_OPENED];
    uint16_t pending[MAX_OPENED], n = 0;
    for (uint16_t i = 0; i < count; ++i) {
      if (ascending[i] == UINT16_MAX && identities[i].fileSize) {
        pending[n] = i;
        unresolved[n++] = {identities[i].pathHash, 0};
      }
    }
    if (n) {
      uint16_t found[MAX_OPENED];
      if (!index.recentRowsFor(unresolved, n, found)) return false;
      for (uint16_t i = 0; i < n; ++i) ascending[pending[i]] = found[i];
    }

    indexSize = header.selfSize;
    bookCount = header.bookCount;
    nextFirstSeen = header.nextFirstSeen;
    openedCount = count;
    hiddenCount = 0;
    for (uint16_t i = 0; i < count; ++i) {
      paths[i] = identities[i].pathHash;
      if (ascending[i] < bookCount) rows[hiddenCount++] = bookCount - 1 - ascending[i];
    }
    std::sort(rows, rows + hiddenCount);
    hiddenCount = std::unique(rows, rows + hiddenCount) - rows;
    return true;
  }
};

CrosspointServices::CrosspointServices(std::string appName, std::string appDir)
    : appName_(std::move(appName)), appDir_(std::move(appDir)), crypto_(new WolfsslCrypto()) {}
CrosspointServices::~CrosspointServices() = default;

// ---- core ----
uint32_t CrosspointServices::timeNow() {
  const time_t t = ::time(nullptr);
  return t > 0 ? static_cast<uint32_t>(t) : 0;
}

// Local wall-clock for home themes (clock + date). The RTC keeps UTC; halClock
// applies the user's timezone. 8 bytes LE: u16 year, u8 month,day,hour,min,sec,wday.
int CrosspointServices::clock(uint8_t* out) {
  struct tm t;
  if (!halClock.localTime(t)) return DSN_ERR_UNAVAILABLE;  // clock unset
  const uint16_t year = static_cast<uint16_t>(t.tm_year + 1900);
  out[0] = static_cast<uint8_t>(year & 0xff);
  out[1] = static_cast<uint8_t>(year >> 8);
  out[2] = static_cast<uint8_t>(t.tm_mon + 1);
  out[3] = static_cast<uint8_t>(t.tm_mday);
  out[4] = static_cast<uint8_t>(t.tm_hour);
  out[5] = static_cast<uint8_t>(t.tm_min);
  out[6] = static_cast<uint8_t>(t.tm_sec);
  out[7] = static_cast<uint8_t>(t.tm_wday);
  return 0;
}

bool CrosspointServices::randomBytes(uint8_t* out, uint32_t n) {
  crypto_->randomBytes(out, n);
  return true;
}

// Per-app device id = hex(sha256(deviceSecret || appName)) — matches the web
// status route, and is unlinkable across apps.
int CrosspointServices::deviceId(char* out, uint32_t cap) {
  uint8_t secret[32];
  if (!deviceSecret(secret)) return DSN_ERR_UNAVAILABLE;
  uint8_t input[32 + 64];
  memcpy(input, secret, 32);
  size_t nameLen;
  if (appName_.size() > 64) {
    // Fold a long name to its full-name digest so names sharing the first 64
    // bytes still get distinct ids (plain truncation would collide them).
    crypto_->sha256(reinterpret_cast<const uint8_t*>(appName_.data()), appName_.size(), input + 32);
    nameLen = 32;
  } else {
    memcpy(input + 32, appName_.data(), appName_.size());
    nameLen = appName_.size();
  }
  uint8_t digest[32];
  crypto_->sha256(input, 32 + nameLen, digest);
  if (65 > cap) return DSN_ERR_TOO_BIG;
  for (size_t i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
  return 64;
}

int CrosspointServices::locale(char* out, uint32_t cap) {
  const char* code = LANGUAGE_CODES[static_cast<int>(I18N.getLanguage())];
  const size_t n = strlen(code);
  if (n + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, code, n + 1);
  return static_cast<int>(n);
}

// ---- storage ----
// The binding already enforced the path rules. App paths are SD paths as-is:
// apps are user-installed from the owner's own card (whole-card policy).
int CrosspointServices::resolve(const char* path, uint32_t maxLen) {
  if (!path || path[0] != '/' || strlen(path) > maxLen) return DSN_ERR_INVALID;
  return 0;
}

int CrosspointServices::fsRead(const char* path, uint8_t* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  HalFile f;
  if (!Storage.openFileForRead("DSN", path, f)) return DSN_ERR_IO;
  const size_t size = f.fileSize();
  if (size > cap) return DSN_ERR_TOO_BIG;
  const int got = f.read(out, size);
  return got == static_cast<int>(size) ? static_cast<int>(size) : DSN_ERR_IO;
}

int CrosspointServices::fsReadAt(const char* path, uint32_t offset, uint8_t* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  HalFile f;
  if (!Storage.openFileForRead("DSN", path, f)) return DSN_ERR_IO;
  if (offset >= f.fileSize()) return 0;
  if (!f.seek(offset)) return DSN_ERR_IO;
  const int got = f.read(out, cap);
  return got < 0 ? DSN_ERR_IO : got;
}

int CrosspointServices::fsWrite(const char* path, const uint8_t* data, uint32_t n) {
  if (int e = resolve(path)) return e;
  HalFile f;
  if (!Storage.openFileForWrite("DSN", path, f)) return DSN_ERR_IO;
  const size_t wrote = f.write(data, n);
  const bool closed = f.close();  // close flushes buffered data; its failure means a short write
  return (wrote == n && closed) ? 0 : DSN_ERR_IO;
}

int CrosspointServices::fsList(const char* path, char* out, uint32_t cap) {
  if (int e = resolve(path)) return e;
  HalFile dir;
  if (!Storage.openFileForRead("DSN", path, dir) || !dir.isDirectory()) return DSN_ERR_IO;
  BoundedOutput entries(cap);
  for (HalFile e = dir.openNextFile(); e; e = dir.openNextFile()) {
    char name[257];
    size_t len = e.getName(name, sizeof name - 1);
    if (len == 0) continue;
    if (e.isDirectory()) name[len++] = '/';
    name[len++] = '\n';
    if (!entries.append(name, len)) return entries.error;
  }
  return entries.copyTo(out);
}

int CrosspointServices::fsDelete(const char* path) {
  if (int e = resolve(path)) return e;
  return Storage.remove(path) ? 0 : DSN_ERR_IO;
}

int CrosspointServices::fsAppDir(char* out, uint32_t cap) {
  if (appDir_.size() + 1 > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, appDir_.c_str(), appDir_.size() + 1);
  return static_cast<int>(appDir_.size());
}

int CrosspointServices::keystore(const char* path, const uint8_t* key, uint32_t n, uint32_t expires) {
  if (int e = resolve(path)) return e;
  if (n > bookkey::MAX_KEY_LEN) return DSN_ERR_INVALID;
  return bookkey::write(path, key, n, expires ? static_cast<int64_t>(expires) : 0) ? 0 : DSN_ERR_IO;
}

// ---- launcher / home ----
int CrosspointServices::recents(char* out, uint32_t cap) {
  std::string acc;
  for (const RecentBook& b : RECENT_BOOKS.getBooks()) {
    char pct[8];
    snprintf(pct, sizeof pct, "%d", loadBookProgress(b.path));  // -1 when unknown
    acc += b.path;
    acc += '\t';
    // Normalize control chars to space (as writeShelfRow does) so a title or
    // author with an embedded tab/newline cannot forge columns or rows.
    for (char c : b.title) acc += shelfControl(c) ? ' ' : c;
    acc += '\t';
    for (char c : b.author) acc += shelfControl(c) ? ' ' : c;
    acc += '\t';
    acc += pct;
    acc += '\n';
  }
  if (acc.size() > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, acc.data(), acc.size());
  return static_cast<int>(acc.size());
}

int CrosspointServices::recentShelf(uint32_t first, uint32_t count, char* out, uint32_t cap) {
  if (count > DSN_SHELF_PAGE_MAX) return DSN_ERR_INVALID;
  if (!count) return 0;

  const RecentBook* opened[ShelfOverlap::MAX_OPENED];
  library::BookIdentity identities[ShelfOverlap::MAX_OPENED];
  uint16_t openedCount = 0;
  for (const RecentBook& book : RECENT_BOOKS.getBooks()) {
    if (openedCount == ShelfOverlap::MAX_OPENED) break;
    if (!shelfPathValid(book.path)) continue;
    auto file = Storage.open(book.path.c_str());
    if (!file || file.isDirectory()) continue;
    identities[openedCount] = {library::clixPathHash(book.path.data(), book.path.size()),
                               static_cast<uint32_t>(file.fileSize())};
    opened[openedCount++] = &book;
  }
  uint32_t used = 0;
  while (first < openedCount && count) {
    const RecentBook& book = *opened[first++];
    const int bytes = writeShelfRow(out + used, cap - used, book.path, book.title, book.author);
    if (bytes < 0) return used ? static_cast<int>(used) : bytes;
    used += bytes;
    --count;
  }
  if (!count) return static_cast<int>(used);

  uint32_t row = first - openedCount;
  library::LibraryIndexFile index;
  const char* indexPath = library::libraryIndexPath();
  if (!index.open(indexPath)) return Storage.exists(indexPath) ? DSN_ERR_IO : static_cast<int>(used);
  if (row >= index.bookCount()) return static_cast<int>(used);
  if (!shelfOverlap_) shelfOverlap_ = makeUniqueNoThrow<ShelfOverlap>();
  if (!shelfOverlap_ || !shelfOverlap_->resolve(index, identities, openedCount)) return DSN_ERR_IO;

  size_t hidden = 0;
  while (hidden < shelfOverlap_->hiddenCount && shelfOverlap_->rows[hidden] <= row) {
    ++hidden;
    ++row;
  }
  uint16_t ordinals[DSN_SHELF_PAGE_MAX];
  uint32_t n = 0;
  for (; n < count && row < index.bookCount(); ++row) {
    if (hidden < shelfOverlap_->hiddenCount && shelfOverlap_->rows[hidden] == row) {
      ++hidden;
      continue;
    }
    ordinals[n++] = index.ordinalForRow(library::SortOrder::RecentDesc, static_cast<uint16_t>(row));
  }
  if (index.ioFailed()) return DSN_ERR_IO;
  if (!n) return static_cast<int>(used);
  auto folders = makeUniqueNoThrow<library::LibraryIndexFile::FolderLookup[]>(n);
  if (!folders) return DSN_ERR_IO;
  library::ClixRecord rec{};
  for (uint32_t i = 0; i < n; ++i) {
    folders[i].id = index.readRecord(ordinals[i], rec) ? rec.folderId : UINT16_MAX;
    if (index.ioFailed()) return DSN_ERR_IO;
  }
  if (!index.readFolders(folders.get(), n)) return DSN_ERR_IO;

  std::string name, title, author;
  for (uint32_t i = 0; i < n; ++i) {
    std::string path;
    if (folders[i].path[0] && index.readRecord(ordinals[i], rec) && index.readName(rec, name)) {
      path = library::joinLibraryPath(folders[i].path, name);
      if (!index.readTitle(rec, title)) title = name;
      index.readAuthor(rec, author);
    }
    if (index.ioFailed()) return DSN_ERR_IO;
    const int bytes = writeShelfRow(out + used, cap - used, path, title, author);
    if (bytes < 0) return used ? static_cast<int>(used) : bytes;
    used += bytes;
  }
  return static_cast<int>(used);
}

int CrosspointServices::bookProgress(const char* path) {
  if (resolve(path, DSN_BOOK_PATH_MAX)) return -1;
  return loadBookProgress(path);
}

// The firmware's fallback cover (drawCoverPlaceholder) as a 1-bit bitmap: a
// white top band with the book glyph, a black body, and a border. Returned by
// bookCover() whenever a book has no usable cover, so every book shows one.
static int packPlaceholderCover(uint8_t* out, uint32_t cap, int w, int h) {
  const uint32_t stride = (static_cast<uint32_t>(w) + 7) / 8;
  const uint32_t need = 4 + stride * static_cast<uint32_t>(h);
  if (need > cap) return DSN_ERR_TOO_BIG;
  out[0] = static_cast<uint8_t>(w);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h);
  out[3] = static_cast<uint8_t>(h >> 8);
  uint8_t* px = out + 4;
  memset(px, 0, stride * static_cast<uint32_t>(h));
  auto set = [&](int x, int y) {
    if (x >= 0 && x < w && y >= 0 && y < h) px[static_cast<uint32_t>(y) * stride + (x >> 3)] |= 0x80 >> (x & 7);
  };
  const int top = h / 3;
  for (int y = top; y < h; ++y)
    for (int x = 0; x < w; ++x) set(x, y);  // black body (bottom two thirds)
  for (int x = 0; x < w; ++x) set(x, 0), set(x, h - 1);
  for (int y = 0; y < h; ++y) set(0, y), set(w - 1, y);  // border
  if (w >= 36 && top >= 36) {                            // book glyph, centred in the top band
    const int ix = (w - 32) / 2, iy = (top - 32) / 2;
    for (int row = 0; row < 32; ++row)
      for (int col = 0; col < 32; ++col)
        if (((CoverIcon[row * 4 + (col >> 3)] >> (7 - (col & 7))) & 1) == 0) set(ix + col, iy + row);  // bit0 = ink
  }
  return static_cast<int>(need);
}

// A book's cover as a dithered 1-bit bitmap scaled to `height`: writes
// {u16 w, u16 h} then the packed MSB-first rows (the dsn_bitmap BW format).
// Generates and caches the thumbnail on first use (the slow part — decoding the
// cover image), so apps request one per redraw. A book with no usable cover
// gets the firmware's placeholder, so it never returns 0 for a real book. PSRAM
// only: the decode/scale buffers are too large for a C3's internal RAM.
int CrosspointServices::bookCover(const char* path, uint32_t height, uint8_t* out, uint32_t cap) {
#ifndef BOARD_HAS_PSRAM
  (void)path, (void)height, (void)out, (void)cap;
  return DSN_ERR_UNAVAILABLE;  // covers need PSRAM for the decode/scale buffers
#else
  if (resolve(path, DSN_BOOK_PATH_MAX)) return DSN_ERR_INVALID;
  const std::string full(path);
  if (height < 16 || height > 512) return DSN_ERR_INVALID;
  const int ph = static_cast<int>(height), pw = ph * 2 / 3;  // placeholder size (book aspect)
  if (!FsHelpers::hasReflowableBookExtension(full)) return packPlaceholderCover(out, cap, pw, ph);
  Epub epub(full, "/.crosspoint");
  const std::string thumb = epub.getThumbBmpPath(static_cast<int>(height));
  if (!Storage.exists(thumb.c_str())) {  // decode + cache on first use (slow)
    const bool cacheLoaded = epub.load(false, true);
    if (!(cacheLoaded ? epub.generateThumbBmp(static_cast<int>(height))
                      : epub.generateThumbBmpFromSource(static_cast<int>(height))))
      return packPlaceholderCover(out, cap, pw, ph);  // no cover image in this book
  }
  HalFile file;
  if (!Storage.openFileForRead("DSN", thumb, file)) return packPlaceholderCover(out, cap, pw, ph);
  Bitmap bmp(file);
  if (bmp.parseHeaders() != BmpReaderError::Ok) return packPlaceholderCover(out, cap, pw, ph);
  const int w = bmp.getWidth(), h = bmp.getHeight();
  if (w <= 0 || h <= 0) return packPlaceholderCover(out, cap, pw, ph);
  const uint32_t stride = (static_cast<uint32_t>(w) + 7) / 8;
  const uint32_t need = 4 + stride * static_cast<uint32_t>(h);
  if (need > cap) return DSN_ERR_TOO_BIG;
  out[0] = static_cast<uint8_t>(w);
  out[1] = static_cast<uint8_t>(w >> 8);
  out[2] = static_cast<uint8_t>(h);
  out[3] = static_cast<uint8_t>(h >> 8);
  uint8_t* px = out + 4;
  memset(px, 0, stride * static_cast<uint32_t>(h));
  static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  std::vector<uint8_t> gray(w), rowbuf(static_cast<size_t>(w) * 4 + 16);
  for (int y = 0; y < h; ++y) {
    // A mid-image row failure would otherwise return a half-zeroed bitmap as
    // complete; fall back to the placeholder like every other failure here.
    if (bmp.readRowGray(y, gray.data(), rowbuf.data()) != BmpReaderError::Ok)
      return packPlaceholderCover(out, cap, pw, ph);
    uint8_t* line = px + static_cast<uint32_t>(y) * stride;
    for (int x = 0; x < w; ++x)
      if (gray[x] < BAYER[y & 3][x & 3] * 16 + 8) line[x >> 3] |= 0x80 >> (x & 7);  // dark -> black bit
  }
  return static_cast<int>(need);
#endif
}

// ---- crypto ----
int CrosspointServices::hash(int algo, const uint8_t* data, uint32_t n, uint8_t* out, uint32_t cap) {
  if (algo == DSN_HASH_SHA1) {
    if (cap < 20) return DSN_ERR_TOO_BIG;
    crypto_->sha1(data, n, out);
    return 20;
  }
  if (algo == DSN_HASH_SHA256) {
    if (cap < 32) return DSN_ERR_TOO_BIG;
    crypto_->sha256(data, n, out);
    return 32;
  }
  return DSN_ERR_INVALID;
}

int CrosspointServices::aesCbc(int flags, const uint8_t* keyiv, const uint8_t* data, uint32_t n, uint8_t* out,
                               uint32_t cap) {
  const uint8_t* key = keyiv;
  const uint8_t* iv = keyiv + 16;
  const bool enc = flags & DSN_AES_ENCRYPT;
  const bool pad = !(flags & DSN_AES_NOPAD);
  if (enc) {
    // Build the (optionally padded) plaintext, then raw-encrypt in place.
    uint32_t len = n;
    if (pad) len = (n & ~15u) + 16;  // PKCS#7 always adds a full block when aligned
    if (len > cap) return DSN_ERR_TOO_BIG;
    if (out != data) memcpy(out, data, n);
    if (pad) memset(out + n, static_cast<uint8_t>(len - n), len - n);
    return crypto_->aes128CbcEncrypt(key, iv, out, len, out) ? static_cast<int>(len) : DSN_ERR_INVALID;
  }
  if (n > cap) return DSN_ERR_TOO_BIG;
  if (!crypto_->aes128CbcDecrypt(key, iv, data, n, out)) return DSN_ERR_INVALID;
  if (!pad) return static_cast<int>(n);
  const int stripped = pkcs7Strip(out, n);
  return stripped < 0 ? DSN_ERR_INVALID : stripped;
}

int CrosspointServices::rsaEncrypt(const uint8_t* cert, uint32_t cn, const uint8_t* data, uint32_t n, uint8_t* out,
                                   uint32_t cap) {
  size_t outLen = 0;
  if (!crypto_->rsaPublicEncrypt(cert, cn, data, n, out, cap, &outLen)) return DSN_ERR_INVALID;
  return static_cast<int>(outLen);
}

int CrosspointServices::rsaDecrypt(int flags, const uint8_t* priv, uint32_t pn, const uint8_t* data, uint32_t n,
                                   uint8_t* out, uint32_t cap) {
  uint8_t block[256];
  const int32_t blockLen = crypto_->rsaPrivateRaw(priv, pn, data, n, block, sizeof block);
  if (blockLen <= 0) return DSN_ERR_INVALID;
  if (flags & DSN_RSA_RAW) {
    if (static_cast<uint32_t>(blockLen) > cap) return DSN_ERR_TOO_BIG;
    memcpy(out, block, blockLen);
    return blockLen;
  }
  // Strip PKCS#1 v1.5 type-2: 00 02 <nonzero padding>=8 00 <message>.
  if (blockLen < 11 || block[0] != 0x00 || block[1] != 0x02) return DSN_ERR_INVALID;
  int sep = -1;
  for (int i = 2; i < blockLen; i++)
    if (block[i] == 0x00) {
      sep = i;
      break;
    }
  if (sep < 10) return DSN_ERR_INVALID;  // need >= 8 padding bytes
  const uint32_t msgLen = blockLen - sep - 1;
  if (msgLen > cap) return DSN_ERR_TOO_BIG;
  memcpy(out, block + sep + 1, msgLen);
  return static_cast<int>(msgLen);
}

int CrosspointServices::rsaSign(int algo, const uint8_t* priv, uint32_t pn, const uint8_t* digest, uint32_t dn,
                                uint8_t* out, uint32_t cap) {
  // The firmware primitive signs a 20-byte (SHA-1) digest, PKCS#1 v1.5, no
  // DigestInfo, into a 128-byte (1024-bit) signature — exactly the one case the
  // first consumer needs. Other hash/DigestInfo combinations are not provided.
  if ((algo & 0x7f) != DSN_HASH_SHA1 || !(algo & DSN_SIGN_NODIGESTINFO)) return DSN_ERR_INVALID;
  if (dn != 20 || cap < 128) return DSN_ERR_TOO_BIG;
  return crypto_->rsaPrivateSignRaw(priv, pn, digest, out) ? 128 : DSN_ERR_INVALID;
}

int CrosspointServices::pkcs12Parse(const uint8_t* p12, uint32_t n, const char* pass, uint8_t* out, uint32_t cap) {
  std::vector<uint8_t> key, cert;
  if (!crypto_->pkcs12Extract(p12, n, pass, &key, &cert)) return DSN_ERR_INVALID;
  if (key.size() >= 0x10000) return DSN_ERR_INVALID;
  const size_t total = 2 + key.size() + cert.size();
  if (total > cap) return DSN_ERR_TOO_BIG;
  out[0] = (key.size() >> 8) & 0xff;
  out[1] = key.size() & 0xff;
  memcpy(out + 2, key.data(), key.size());
  memcpy(out + 2 + key.size(), cert.data(), cert.size());
  return static_cast<int>(total);
}

// ---- board ----
int CrosspointServices::backlightSet(int brightness, int warm) {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  if (brightness >= 0) Frontlight.setBrightness(static_cast<uint8_t>(brightness > 100 ? 100 : brightness));
  if (warm >= 0 && Frontlight.hasColorTemperature())
    Frontlight.setWarmth(static_cast<uint8_t>(warm > 100 ? 100 : warm));
  return 0;
}

int CrosspointServices::backlightGet() {
  if (!Frontlight.present()) return DSN_ERR_UNAVAILABLE;
  return Frontlight.brightness() | (Frontlight.hasColorTemperature() ? Frontlight.warmth() : 0) << 8;
}

int CrosspointServices::battery() {
  const uint16_t pct = powerManager.getBatteryPercentage();
  if (pct > 100) return DSN_ERR_UNAVAILABLE;
  // Second byte: power state, from the same external-power signal the status bar
  // uses (board-aware: USB-detect pin, charger IC, or charging state). Some
  // boards read "not connected" at charge termination, so a full battery there
  // reports BATTERY — matching the status bar, which drops its bolt the same way.
  uint8_t state;
  if (!gpio.isUsbConnected())
    state = DSN_PWR_BATTERY;
  else if (pct >= 100)
    state = DSN_PWR_FULL;
  else
    state = DSN_PWR_CHARGING;
  return (pct & 0xff) | state << 8;
}

int CrosspointServices::haptic(int ms, int intensity) {
  (void)ms;  // HalHaptics plays a fixed short pattern; duration is advisory
  HalHaptics::longPress(true, static_cast<uint8_t>(intensity > 2 ? 2 : intensity < 0 ? 0 : intensity));
  return 0;
}

int CrosspointServices::accel(uint8_t*) {
  // The firmware tilt HAL exposes gyro rate, not acceleration in g. Until a
  // board surfaces raw accel, report "no such part".
  return DSN_ERR_UNAVAILABLE;
}

// ---- async: queue on the render task, execute on the loop task ----
int CrosspointServices::wifiConnectStart() {
  pending_ = Op::WifiStart;
  done_ = false;
  return dessen::kPending;
}

int CrosspointServices::netStart(const char* head, const uint8_t* body, uint32_t bn, uint8_t* resp, uint32_t cap) {
  pending_ = Op::Net;
  done_ = false;
  head_ = head;
  body_ = body;
  bodySize_ = bn;
  out_ = resp;
  outCap_ = cap;
  return dessen::kPending;
}

int CrosspointServices::fetchStart(const char* url, const char* dest) {
  pending_ = Op::Fetch;
  done_ = false;
  url_ = url;
  dest_ = dest;
  return dessen::kPending;
}

int CrosspointServices::keygenStart(uint32_t bits, uint8_t* pub, uint32_t pubCap, uint8_t* priv, uint32_t privCap) {
  pending_ = Op::Keygen;
  done_ = false;
  bits_ = bits;
  out_ = pub;
  outCap_ = pubCap;
  priv_ = priv;
  privCap_ = privCap;
  return dessen::kPending;
}

int CrosspointServices::textInputStart(const char* prompt, char* out, uint32_t cap) {
  pending_ = Op::TextInput;
  done_ = false;
  prompt_ = prompt;
  out_ = reinterpret_cast<uint8_t*>(out);
  outCap_ = cap;
  return dessen::kPending;
}

int CrosspointServices::poll() {
  if (!done_) return dessen::kPending;
  pending_ = Op::None;
  return result_;
}

bool CrosspointServices::serviceAsyncWork() {
  if (done_) return true;
  switch (pending_) {
    case Op::Net: {
      const std::string_view request = head_;
      const size_t newline = request.find('\n');
      const auto first = request.substr(0, newline);
      const size_t space = first.find(' ');
      if (space == std::string_view::npos) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      const std::string method(first.substr(0, space));
      auto url = first.substr(space + 1);
      if (url.ends_with('\r')) url.remove_suffix(1);
      if ((method != "GET" && method != "POST" && method != "PUT") || url.empty() ||
          std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c == 127; })) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      if (outCap_ < 4) {
        result_ = DSN_ERR_TOO_BIG;
        break;
      }
      std::vector<HttpDownloader::Header> headers;
      result_ = 0;
      for (size_t pos = newline; pos != std::string_view::npos && ++pos < request.size();) {
        const size_t end = request.find('\n', pos);
        auto line = request.substr(pos, end == std::string_view::npos ? end : end - pos);
        pos = end;
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.empty() && (end == std::string_view::npos || end + 1 == request.size())) break;
        const size_t colon = line.find(':');
        const auto name = line.substr(0, colon);
        if (colon == std::string_view::npos || name.empty() ||
            name.find_first_not_of("!#$%&'*+-.^_`|~0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz") !=
                std::string_view::npos) {
          result_ = DSN_ERR_INVALID;
          break;
        }
        auto value = line.substr(colon + 1);
        if (std::any_of(value.begin(), value.end(),
                        [](unsigned char c) { return (c < 32 && c != '\t') || c == 127; })) {
          result_ = DSN_ERR_INVALID;
          break;
        }
        value.remove_prefix(std::min(value.find_first_not_of(" \t"), value.size()));
        headers.emplace_back(name, value);
        const char* headerName = headers.back().first.c_str();
        if (strcasecmp(headerName, "Host") == 0 || strcasecmp(headerName, "Connection") == 0 ||
            strcasecmp(headerName, "Content-Length") == 0 || strcasecmp(headerName, "Transfer-Encoding") == 0) {
          result_ = DSN_ERR_INVALID;
          break;
        }
      }
      if (result_ < 0) break;

      // TLS needs a healthy heap; refuse up front rather than OOM mid-handshake
      // (same thresholds as CatalogActivity / FontDownloadActivity).
      if (url.size() >= 6 && strncasecmp(url.data(), "https:", 6) == 0 &&
          (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
           ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC)) {
        result_ = DSN_ERR_IO;
        break;
      }

      // DNS and sockets require an initialized, connected network stack.
      if (WiFi.status() != WL_CONNECTED) {
        result_ = DSN_ERR_IO;
        break;
      }
      BoundedOutput response(outCap_ - 4);
      const int status =
          HttpDownloader::request(method.c_str(), std::string(url), headers, body_, bodySize_,
                                  [&](const uint8_t* bytes, size_t len) { return response.append(bytes, len); });
      if (status == 0) {
        result_ = response.error ? response.error : DSN_ERR_IO;
        break;
      }
      out_[0] = (status >> 8) & 0xff;
      out_[1] = status & 0xff;
      out_[2] = 0;
      out_[3] = 0;
      result_ = response.copyTo(out_ + 4);
      break;
    }
    case Op::Fetch: {
      size_t bytes = 0;
      if (strncasecmp(url_.c_str(), "https:", 6) == 0 && (ESP.getFreeHeap() < HttpDownloader::MIN_TLS_FREE_HEAP ||
                                                          ESP.getMaxAllocHeap() < HttpDownloader::MIN_TLS_MAX_ALLOC)) {
        result_ = DSN_ERR_IO;
        break;
      }
      // DNS and sockets require an initialized, connected network stack.
      if (WiFi.status() != WL_CONNECTED) {
        result_ = DSN_ERR_IO;
        break;
      }
      const auto error = HttpDownloader::downloadToFile(url_, dest_, nullptr, nullptr, "", "", {}, false, &bytes,
                                                        dessen::kPending - 1);
      result_ = error == HttpDownloader::OK        ? static_cast<int>(bytes)
                : error == HttpDownloader::TOO_BIG ? DSN_ERR_TOO_BIG
                                                   : DSN_ERR_IO;
      break;
    }
    case Op::Keygen: {
      if (bits_ != 1024) {
        result_ = DSN_ERR_INVALID;
        break;
      }
      RsaKeyPairDer pair;
      if (!crypto_->rsaGenerate(&pair)) {
        result_ = DSN_ERR_IO;
        break;
      }
      if (pair.spki.size() > outCap_ || pair.pkcs8.size() > privCap_) {
        result_ = DSN_ERR_TOO_BIG;
        break;
      }
      memcpy(out_, pair.spki.data(), pair.spki.size());
      memcpy(priv_, pair.pkcs8.data(), pair.pkcs8.size());
      result_ = static_cast<int>(pair.spki.size()) << 16 | static_cast<int>(pair.pkcs8.size());
      break;
    }
    case Op::WifiStart: {
      if (WiFi.status() == WL_CONNECTED) {
        result_ = 0;
        break;
      }
      // The launcher can be the first Wi-Fi user after boot.
      WIFI_STORE.loadFromFile();
      const auto last = WIFI_STORE.getLastConnectedSsid();
      auto cred = last.empty() ? std::nullopt : WIFI_STORE.findCredential(last);
      if (!cred) cred = WIFI_STORE.getCredentialAt(0);
      if (!cred) {
        result_ = DSN_ERR_UNAVAILABLE;
        break;
      }
      WiFi.persistent(false);
      LOG_INF("DSN", "Wi-Fi start: free=%u max_block=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      if (!WiFi.mode(WIFI_STA)) {
        result_ = DSN_ERR_IO;
        break;
      }
      WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
      wifiStart_ = millis();
      pending_ = Op::WifiConnecting;
      return false;
    }
    case Op::WifiConnecting:
      if (WiFi.status() == WL_CONNECTED) {
        result_ = 0;
      } else if (millis() - wifiStart_ > 20000) {
        result_ = DSN_ERR_IO;
      } else {
        return false;
      }
      break;
    case Op::None:
    case Op::TextInput:
      return false;
  }
  done_ = true;
  return true;
}

void CrosspointServices::deliverText(const char* text) {
  const size_t n = strlen(text);
  if (n + 1 > outCap_) {
    result_ = DSN_ERR_TOO_BIG;
  } else {
    memcpy(out_, text, n + 1);
    result_ = static_cast<int>(n);
  }
  done_ = true;
}

void CrosspointServices::cancelText() {
  result_ = -1;  // text_input returns -1 on cancel
  done_ = true;
}

#endif  // CROSSPOINT_DESSEN_APPS

// The theme draws its hints upright (Portrait) along the bottom, and none on a
// touch board, so that is the only case with a band to report.
int CrosspointServices::buttonHintsHeight() {
  if (!hintRenderer_ || gpio.hasTouch() || hintRenderer_->getOrientation() != GfxRenderer::Orientation::Portrait) {
    return 0;
  }
  return UITheme::getInstance().getMetrics().buttonHintsHeight;
}

void CrosspointServices::drawButtonHints(freeink::ui::DrawTarget&, const freeink::ui::Rect&,
                                         const char* const labels[4]) {
  if (!hintRenderer_ || !hintInput_) return;
  // The same remapping every firmware screen uses: the app names actions,
  // the user's settings say which key each is on.
  const auto keys = hintInput_->mapLabels(labels[0], labels[1], labels[2], labels[3]);
  GUI.drawButtonHints(*hintRenderer_, keys.btn1, keys.btn2, keys.btn3, keys.btn4);
}
