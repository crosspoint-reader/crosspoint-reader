// SD/HAL binding for the content-protection read path.
//
// The ContentProtection SDK lib is storage-agnostic (it works against a
// ByteSource). This file is the firmware-side glue that backs that seam with
// the device's SD storage: a HalStorage-backed ByteSource, the credential
// lookup, and the openProtectedBook() entry point the reader calls. It lives in
// the firmware — not the SDK lib — so the portable lib carries no HAL dependency.

#include <Arduino.h>
#include <ByteSource.h>
#include <ContentProtection.h>
#include <Credential.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryManager.h>
#include <ProtectedBook.h>
#include <TrustedTime.h>
#include <WolfsslCrypto.h>
#include <Zip.h>
#include <esp_heap_caps.h>

namespace freeink {
namespace content {

namespace {

// The access credential is provisioned off-device and dropped here.
// Generic path — the reader carries no scheme name.
constexpr const char* kCredentialPath = "/.crosspoint/content.key";

// One shared crypto backend for the whole read path.
WolfsslCrypto& crypto() {
  static WolfsslCrypto instance;
  return instance;
}

// ByteSource over an SD file (read-only). One open handle per instance.
class SdByteSource : public ByteSource {
 public:
  explicit SdByteSource(std::string path) : path_(std::move(path)) {}
  bool open() {
    file_ = Storage.open(path_.c_str(), O_RDONLY);
    return file_ && file_.isOpen();
  }
  // Open once, then reuse across reads — decrypting a book faults many entries.
  bool ensureOpen() { return (file_ && file_.isOpen()) || open(); }
  int32_t readAt(uint64_t offset, void* dst, uint32_t len) override {
    if (!file_ || !file_.seek64(offset)) return -1;
    return file_.read(dst, len);
  }
  uint64_t size() const override { return file_ ? file_.fileSize64() : 0; }

 private:
  std::string path_;
  mutable HalFile file_;
};

// Adapts an opened ProtectedBook to the reader-facing access interface.
class ProtectedBookDecryptor : public ContentDecryptor {
 public:
  ProtectedBookDecryptor(std::string epubPath, std::unique_ptr<ProtectedBook> book)
      : source_(std::move(epubPath)), book_(std::move(book)) {}

  bool isEncrypted(const std::string& itemPath) const override { return book_->isEncrypted(itemPath); }

  size_t decryptedSize(const std::string& itemPath) const override { return book_->decryptedSize(itemPath); }

  bool decryptToSink(const std::string& itemPath, ContentChunkSink sink, void* context) override {
    // Reuse one open SD handle for the whole reader session rather than
    // reconstructing and reopening it per encrypted entry.
    if (!source_.ensureOpen()) return false;
    // miniz's inflate state (allocated inside decryptEntryToSink) embeds a
    // single ~33KB contiguous window. SD card fonts hold large resident blocks
    // that fragment the heap below that, so the per-chapter decrypt fails and
    // the reader shows "Invalid book / DRM protected file" (sd-plugins #14).
    // When the largest free block is marginal, evict rebuildable caches (the
    // SD-font mini tables, via the sinks GfxRenderer registers) to reclaim a
    // contiguous window; fonts fault back in on the next measurement pass.
    constexpr size_t kInflateWindow = 40 * 1024;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) < kInflateWindow) {
      freeink::MemoryManager::instance().ensureFree(kInflateWindow);
    }
    return book_->decryptEntryToSink(source_, crypto(), itemPath, sink, context);
  }

 private:
  SdByteSource source_;
  std::unique_ptr<ProtectedBook> book_;
};

}  // namespace

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err) {
  err.clear();
  // Field-report heap ledger: this is where tight-heap opens historically
  // died; the pair below tells fragmentation (largest collapses) from a leak.
  LOG_INF("CPRO", "open: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // open() is the single existence test: a missing path fails to open.
  SdByteSource source(epubPath);
  if (!source.open()) return nullptr;

  // Classify before initializing crypto or loading credentials, then transfer
  // this same ZIP index into ProtectedBook. Protected EPUBs still scan only
  // once; plain EPUBs return immediately through the normal reader path.
  ZipScan scan;
  if (!scan.open(source) || !scan.find("META-INF/encryption.xml")) return nullptr;

  // A book carrying encryption.xml may only obfuscate its embedded fonts
  // (not content-protected). The SDK demands the credential only after parsing
  // the manifest and finding genuinely encrypted entries.
  SdByteSource credSource(kCredentialPath);
  Credential credential;
  const bool haveCredential = credSource.open() && parseCredential(credSource, &credential);

  auto book = makeUniqueNoThrow<ProtectedBook>();
  if (!book) {
    err = "out of memory";
    return nullptr;
  }
  // Prefer an out-of-band rights document delivered as a sidecar next to the
  // EPUB ("<book>.epub.rights"), so the EPUB on disk stays byte-identical to
  // what the server sent. Falls back to a rights.xml injected into the zip.
  std::string rightsOverride;
  {
    // A real rights document is a few KB; 64KB is a generous ceiling. The
    // largest-block check keeps the resize below from aborting on OOM (string
    // growth is a bare allocation under -fno-exceptions).
    constexpr uint64_t kMaxRightsSize = 64 * 1024;
    SdByteSource rightsSource(epubPath + ".rights");
    if (rightsSource.open()) {
      const uint64_t rsize = rightsSource.size();
      if (rsize > 0 && rsize <= kMaxRightsSize &&
          heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > static_cast<size_t>(rsize) + 8 * 1024) {
        rightsOverride.resize(static_cast<size_t>(rsize));
        const int32_t rn = rightsSource.readAt(0, rightsOverride.data(), static_cast<uint32_t>(rsize));
        if (rn <= 0)
          rightsOverride.clear();
        else
          rightsOverride.resize(static_cast<size_t>(rn));
      }
    }
  }
  if (!book->openFromScan(source, crypto(), credential, std::move(scan), rightsOverride)) {
    if (haveCredential) {
      // Guarded concat: this path runs precisely when the heap is tight, and
      // the temporary would abort under -fno-exceptions.
      err = "cannot open protected content";
      const std::string& detail = book->lastError();
      if (!detail.empty() && heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) > detail.size() + err.size() + 1024) {
        err += ": ";
        err += detail;
      }
    } else {
      err = "no content access key on this device";
    }
    return nullptr;
  }
  // An encryption manifest containing only font obfuscation does not require
  // this read path; let the reader open it normally.
  if (!book->isProtected()) return nullptr;

  // Loan enforcement. The clock is a persisted monotonic floor (TrustedTime):
  // it can lag real time while the device sat powered off, but can never be
  // rolled back — staying offline delays the due date at most by the
  // powered-off gap, it does not suspend it. A book carrying a due date with
  // no trustworthy clock at all fails closed rather than open.
  // Exact err strings below are matched by the reader for the user message.
  if (book->expiresAt() != 0) {
    const int64_t now = trustedtime::trustedNow();
    if (now == 0) {
      err = "loan date unverified";
      return nullptr;
    }
    if (book->isExpired(now)) {
      err = "access expired";
      return nullptr;
    }
  }

  auto decryptor = makeUniqueNoThrow<ProtectedBookDecryptor>(epubPath, std::move(book));
  if (!decryptor) err = "out of memory";
  return decryptor;
}

}  // namespace content
}  // namespace freeink
