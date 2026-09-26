// Indexing a book with thousands of chapters under a device-sized heap. Drives the
// real OPF, nav and book.bin passes in the order Epub::load runs them, over an
// in-memory SD card, and charges every operator new against a cap (HeapCap.h).
#include <BookMetadataCache.h>
#include <ContentOpfParser.h>
#include <HeapCap.h>
#include <TocNavParser.h>
#include <ZipFile.h>
#include <gtest/gtest.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace {
const std::string cachePath = "/cache/book";
const std::string basePath = "OEBPS/";
const std::string epubPath = "/books/huge.epub";

// Heap left for indexing when a book is opened: an X3 leaving Home to open a
// 5,000-chapter book reported 116,012 bytes free (largest block 90,100). The model
// counts only operator new, so a few KB are left for expat and the SD buffers.
constexpr size_t OPEN_HEAP = 110 * 1024;
// More than an ESP32-C3 reader has free with a book open, to show the outcome does
// not hang on the exact figure above.
constexpr size_t GENEROUS_HEAP = 200 * 1024;

std::string pad5(const int i) {
  char buf[8];
  snprintf(buf, sizeof(buf), "%05d", i);
  return buf;
}

uint32_t chapterBytes(const int i) { return 8000 + static_cast<uint32_t>(i * 37 % 9000); }

struct Book {
  std::string opf, nav;
  std::vector<std::pair<std::string, uint32_t>> zip;
};

// n chapters, one XHTML file each; every tocEvery-th chapter has a contents entry.
Book makeBook(const int n, const int tocEvery = 1) {
  heapcap::Untracked guard;
  Book b;
  std::string manifest =
      "<item id=\"nav\" href=\"nav.xhtml\" media-type=\"application/xhtml+xml\" properties=\"nav\"/>";
  std::string spine, items;
  b.zip.push_back({"mimetype", 20});
  b.zip.push_back({"META-INF/container.xml", 200});
  for (int i = 1; i <= n; ++i) {
    const std::string file = "c" + pad5(i) + ".xhtml";
    manifest += "<item id=\"c" + std::to_string(i) + "\" href=\"" + file + "\" media-type=\"application/xhtml+xml\"/>";
    spine += "<itemref idref=\"c" + std::to_string(i) + "\"/>";
    if ((i - 1) % tocEvery == 0) {
      items += "<li><a href=\"" + file + "\">Chapter " + std::to_string(i) + "</a></li>";
    }
    b.zip.push_back({basePath + file, chapterBytes(i)});
  }
  b.zip.push_back({basePath + "nav.xhtml", 400});
  b.zip.push_back({basePath + "content.opf", 400});
  b.opf =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><package xmlns=\"http://www.idpf.org/2007/opf\" version=\"3.0\">"
      "<metadata xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:title>Test</dc:title></metadata><manifest>" +
      manifest + "</manifest><spine>" + spine + "</spine></package>";
  b.nav =
      "<?xml version=\"1.0\" encoding=\"utf-8\"?><html xmlns=\"http://www.w3.org/1999/xhtml\" "
      "xmlns:epub=\"http://www.idpf.org/2007/ops\"><head><title>Contents</title></head><body>"
      "<nav epub:type=\"toc\"><ol>" +
      items + "</ol></nav></body></html>";
  return b;
}

template <typename P>
bool feed(P& parser, const std::string& xml) {
  // The same 1 KB slices Epub hands the parsers from the zip inflater.
  for (size_t at = 0; at < xml.size(); at += 1024) {
    const size_t n = std::min<size_t>(1024, xml.size() - at);
    if (parser.write(reinterpret_cast<const uint8_t*>(xml.data() + at), n) != n) return false;
  }
  return true;
}

struct IndexRun {
  bool ok = false;
  std::string failedAt;
  unsigned aborts = 0;
  std::string abortPhase;
  size_t abortRequest = 0;
  size_t peakOpf = 0, peakToc = 0, peakBookBin = 0, peakLoad = 0;
  size_t zipScans = 0;
};

// Mirrors the cache-building half of Epub::load, then loads the result.
IndexRun indexBook(const Book& book, const size_t cap) {
  {
    heapcap::Untracked guard;
    Storage.files.clear();
    zipModel = {};
    zipModel.entries = book.zip;
  }
  IndexRun run;
  std::string phase = "opf";
  heapcap::reset(cap);
  auto endPhase = [&](size_t& peak) {
    peak = heapcap::peak();
    if (heapcap::aborts() && run.abortPhase.empty()) {
      heapcap::Untracked guard;
      run.abortPhase = phase;
      run.abortRequest = heapcap::firstAbortSize();
    }
    heapcap::resetPeak();
  };
  auto finish = [&](const char* failedAt) {
    run.aborts = heapcap::aborts();
    run.zipScans = zipModel.scans;
    heapcap::stop();
    run.failedAt = failedAt;
    run.ok = run.failedAt.empty();
    return run;
  };

  {
    BookMetadataCache cache(cachePath);
    BookMetadataCache::BookMetadata metadata;
    if (!cache.beginWrite() || !cache.beginContentOpfPass()) return finish("begin");
    {
      ContentOpfParser opf(cachePath, basePath, book.opf.size(), &cache);
      const bool parsed = opf.setup() && feed(opf, book.opf);
      if (!parsed) {
        endPhase(run.peakOpf);
        return finish("opf");
      }
      metadata.title = opf.title;
    }
    if (!cache.endContentOpfPass()) return finish("opf-end");
    endPhase(run.peakOpf);

    phase = "toc";
    if (!cache.beginTocPass()) {
      endPhase(run.peakToc);
      return finish("toc");
    }
    {
      TocNavParser nav(basePath, book.nav.size(), &cache);
      if (!nav.setup() || !feed(nav, book.nav)) {
        endPhase(run.peakToc);
        return finish("toc-parse");
      }
    }
    if (!cache.endTocPass() || !cache.endWrite()) return finish("toc-end");
    endPhase(run.peakToc);

    phase = "bookbin";
    const bool built = cache.buildBookBin(epubPath, metadata);
    endPhase(run.peakBookBin);
    if (!built) return finish("bookbin");
    cache.cleanupTmpFiles();
  }

  phase = "load";
  heapcap::resetPeak();
  BookMetadataCache loaded(cachePath);
  const bool ok = loaded.load();
  endPhase(run.peakLoad);
  return finish(ok ? "" : "load");
}

void print(const int n, const size_t cap, const IndexRun& r) {
  printf(
      "HUGE_INDEX n=%d cap=%zu ok=%d failed_at=%s aborts=%u abort_phase=%s abort_request=%zu peak_opf=%zu "
      "peak_toc=%zu peak_bookbin=%zu peak_load=%zu zip_scans=%zu\n",
      n, cap == SIZE_MAX ? 0 : cap, r.ok ? 1 : 0, r.failedAt.empty() ? "-" : r.failedAt.c_str(), r.aborts,
      r.abortPhase.empty() ? "-" : r.abortPhase.c_str(), r.abortRequest, r.peakOpf, r.peakToc, r.peakBookBin,
      r.peakLoad, r.zipScans);
}

std::vector<uint8_t> bookBinBytes() {
  heapcap::Untracked guard;
  const auto it = Storage.files.find(cachePath + "/book.bin");
  return it == Storage.files.end() ? std::vector<uint8_t>{} : it->second->bytes;
}

// Every spine entry carries the running total of the inflated sizes and the first
// contents entry pointing at it (or, without one, the previous chapter's entry).
void expectCache(const int n, const int tocEvery = 1) {
  BookMetadataCache cache(cachePath);
  ASSERT_TRUE(cache.load());
  ASSERT_EQ(cache.getSpineCount(), n);
  ASSERT_EQ(cache.getTocCount(), (n + tocEvery - 1) / tocEvery);
  uint32_t total = 0;
  for (int i = 0; i < n; ++i) {
    total += chapterBytes(i + 1);
    ASSERT_EQ(cache.getCumulativeSize(i), total) << "spine " << i;
  }
  for (int i : {0, 1, 2, n / 2, n - 2, n - 1}) {
    const auto spine = cache.getSpineEntry(i);
    EXPECT_EQ(spine.href, basePath + "c" + pad5(i + 1) + ".xhtml");
    EXPECT_EQ(spine.tocIndex, i / tocEvery) << "spine " << i;
  }
}
}  // namespace

TEST(HugeBookIndex, FiveThousandChaptersIndexUnderOpenHeap) {
  const Book book = makeBook(5000);
  const IndexRun r = indexBook(book, OPEN_HEAP);
  print(5000, OPEN_HEAP, r);
  EXPECT_EQ(r.aborts, 0u) << "the device would abort in " << r.abortPhase;
  ASSERT_TRUE(r.ok) << r.failedAt;
  expectCache(5000);
}

// Past what the heap can hold, the build must still end in a book that loads or a
// clean refusal that leaves no book.bin behind. Each pass that grows with the
// chapter count (OPF, TOC, book.bin) is where one of these sizes stops.
TEST(HugeBookIndex, LargerBooksIndexOrAreRefusedWithoutAbort) {
  const std::vector<std::pair<int, size_t>> cases = {
      {1500, 44 * 1024},  {6000, OPEN_HEAP},      {7000, OPEN_HEAP},      {9000, OPEN_HEAP},
      {20000, OPEN_HEAP}, {10000, GENEROUS_HEAP}, {20000, GENEROUS_HEAP}, {32000, GENEROUS_HEAP}};
  for (const auto& [n, cap] : cases) {
    const IndexRun r = indexBook(makeBook(n), cap);
    print(n, cap, r);
    EXPECT_EQ(r.aborts, 0u) << n << " chapters: the device would abort in " << r.abortPhase;
    if (r.ok) {
      expectCache(n);
    } else {
      EXPECT_TRUE(bookBinBytes().empty()) << n << " chapters: refused, but a book.bin was left";
    }
  }
}

// A book.bin written in several chunks is byte for byte the one written in one. Only
// every 1,000th chapter has a contents entry, so the chapters after a chunk edge take
// theirs from the previous chunk.
TEST(HugeBookIndex, ChunkedBookBinMatchesOneChunk) {
  const Book book = makeBook(5000, 1000);
  const IndexRun whole = indexBook(book, SIZE_MAX);
  ASSERT_TRUE(whole.ok) << whole.failedAt;
  const std::vector<uint8_t> expected = bookBinBytes();

  const IndexRun chunked = indexBook(book, OPEN_HEAP);
  print(5000, OPEN_HEAP, chunked);
  EXPECT_EQ(chunked.aborts, 0u) << "the device would abort in " << chunked.abortPhase;
  ASSERT_TRUE(chunked.ok) << chunked.failedAt;
  EXPECT_GE(chunked.zipScans, 2u) << "the capped build should take more than one chunk";
  EXPECT_TRUE(bookBinBytes() == expected);
  expectCache(5000, 1000);
}

// Books that fit one chunk keep their single zip directory scan (none below the
// batch lookup threshold).
TEST(HugeBookIndex, OrdinaryBooksKeepOneDirectoryScan) {
  for (const int n : {300, 400, 2000}) {
    const IndexRun r = indexBook(makeBook(n), OPEN_HEAP);
    print(n, OPEN_HEAP, r);
    EXPECT_EQ(r.aborts, 0u);
    ASSERT_TRUE(r.ok) << r.failedAt;
    EXPECT_EQ(r.zipScans, n >= 400 ? 1u : 0u);
    expectCache(n);
  }
}
