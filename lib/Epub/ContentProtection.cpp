// SD/HAL binding for the content-protection read path.
//
// The ContentProtection SDK lib is storage-agnostic (it works against a
// ByteSource). This file is the firmware-side glue that backs that seam with
// the device's SD storage: a HalStorage-backed ByteSource and the
// openProtectedBook() entry point the reader calls. The book's content key
// comes from its device-wrapped "<book>.key" (BookKey.h), written when the
// book was fulfilled; the reader carries no rights or account scheme.

#include <Arduino.h>
#include <ByteSource.h>
#include <ContentProtection.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryManager.h>
#include <ProtectedBook.h>
#include <TrustedTime.h>
#include <WolfsslCrypto.h>
#include <Zip.h>
#include <ZipFile.h>
#include <esp_heap_caps.h>

#include "BookKey.h"
#include "Epub/parsers/EncryptionManifestProbe.h"

namespace freeink {
namespace content {

namespace {

// One shared crypto backend for the whole read path.
WolfsslCrypto& crypto() {
  static WolfsslCrypto instance;
  return instance;
}

void reclaimContentCaches() {
  // miniz needs a contiguous inflate state plus stream buffers and metadata.
  // ensureFree() checks total bytes and cannot detect a fragmented heap.
  constexpr size_t CONTENT_WORKING_SET = 64 * 1024;
  const size_t before = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  if (before >= CONTENT_WORKING_SET) return;
  freeink::MemoryManager::instance().clearCaches();
  LOG_DBG("CPRO", "Cache reclaim: max_block=%u -> %u, free=%u", static_cast<unsigned>(before),
          static_cast<unsigned>(ESP.getMaxAllocHeap()), static_cast<unsigned>(ESP.getFreeHeap()));
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
    reclaimContentCaches();
    if (!book_->decryptEntryToSink(source_, crypto(), itemPath, sink, context)) {
      LOG_ERR("CPRO", "Decrypt failed: %s (%s), free=%u max_block=%u", itemPath.c_str(), book_->lastError().c_str(),
              static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
      return false;
    }
    return true;
  }

 private:
  SdByteSource source_;
  std::unique_ptr<ProtectedBook> book_;
};

}  // namespace

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err) {
  err.clear();
  // Heap at open for crash reports: free vs largest block tells fragmentation
  // (largest collapses) from a leak.
  LOG_INF("CPRO", "open: free=%u max_block=%u", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());

  // The reader's ZIP lookup scans without retaining a directory index.
  size_t manifestSize = 0;
  if (!ZipFile(epubPath).getInflatedFileSize("META-INF/encryption.xml", &manifestSize)) return nullptr;
  {
    EncryptionManifestProbe manifest;
    if (!manifest.setup() || !ZipFile(epubPath).readFileToStream("META-INF/encryption.xml", manifest, 512) ||
        !manifest.finish()) {
      LOG_ERR("CPRO", "Cannot read encryption manifest");
      err = "cannot read encryption manifest";
      return nullptr;
    }
    if (!manifest.needsProtectionCheck()) return nullptr;
  }

  SdByteSource source(epubPath);
  reclaimContentCaches();
  ZipScan scan;
  if (!source.open() || !scan.open(source)) {
    LOG_ERR("CPRO", "Cannot index protected container");
    err = "cannot index protected content";
    return nullptr;
  }

  auto book = makeUniqueNoThrow<ProtectedBook>();
  if (!book) {
    err = "out of memory";
    return nullptr;
  }
  if (!book->openFromScan(source, std::move(scan))) {
    err = "cannot open protected content";
    return nullptr;
  }
  // An encryption manifest containing only font obfuscation does not require
  // this read path; let the reader open it normally.
  if (!book->isProtected()) return nullptr;

  uint8_t key[bookkey::MAX_KEY_LEN];
  int64_t expiresAt = 0;
  const size_t keyLen = bookkey::read(epubPath, key, &expiresAt);
  if (keyLen == 0) {
    err = "no content key for this book";
    return nullptr;
  }
  book->setContentKey(key, keyLen);

  // Loan enforcement. The clock is a persisted monotonic floor (TrustedTime):
  // it can lag real time while the device sat powered off, but can never be
  // rolled back — staying offline delays the due date at most by the
  // powered-off gap, it does not suspend it. A book carrying a due date with
  // no trustworthy clock at all fails closed rather than open.
  // Exact err strings below are matched by the reader for the user message.
  if (expiresAt != 0) {
    const int64_t now = trustedtime::trustedNow();
    if (now == 0) {
      err = "loan date unverified";
      return nullptr;
    }
    if (now > expiresAt) {
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
