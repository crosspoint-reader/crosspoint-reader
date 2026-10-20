#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_set>
#include <vector>

#include "../../lib/Epub/Epub/BookMetadataCache.h"
#include "../../lib/Epub/Epub/parsers/ContentOpfParser.h"
#include "../../lib/Serialization/BufferedFileIO.h"

namespace {

std::string makeTempDir() {
  std::error_code ec;
  const std::filesystem::path tempRoot = std::filesystem::temp_directory_path(ec);
  if (ec) {
    return {};
  }

  std::random_device rd;
  std::array<uint32_t, 4> parts = {rd(), rd(), rd(), rd()};
  for (int attempt = 0; attempt < 8; ++attempt) {
    const std::filesystem::path base =
        tempRoot / ("opf-test-" + std::to_string(parts[0]) + "-" + std::to_string(parts[1]) + "-" +
                    std::to_string(parts[2]) + "-" + std::to_string(parts[3]) + "-" + std::to_string(attempt));
    ec.clear();
    if (std::filesystem::create_directory(base, ec)) {
      return base.string();
    }

    // Retry only on collisions; other filesystem errors should fail fast.
    if (ec && ec != std::errc::file_exists) {
      return {};
    }
  }

  return {};
}

struct TempDirGuard {
  explicit TempDirGuard(std::string path) : path(std::move(path)) {}
  ~TempDirGuard() {
    if (path.empty()) {
      return;
    }
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }

  std::string path;
};

bool parseOpfXml(ContentOpfParser& parser, const std::string& xml) {
  if (!parser.setup()) {
    return false;
  }

  const size_t mid = xml.size() / 2;
  const auto* data = reinterpret_cast<const uint8_t*>(xml.data());
  const size_t first = parser.write(data, mid);
  const size_t second = parser.write(data + mid, xml.size() - mid);
  return first == mid && second == xml.size() - mid;
}

std::string repeatedChar(const char c, const size_t count) { return std::string(count, c); }

std::string buildManifestItems(const int count) {
  std::string out;
  out.reserve(static_cast<size_t>(count) * 80);
  for (int i = 0; i < count; ++i) {
    out += "<item id='ch" + std::to_string(i) + "' href='text/ch" + std::to_string(i) +
           ".xhtml' media-type='application/xhtml+xml'/>";
  }
  return out;
}

}  // namespace

namespace opf_test_hooks {
extern std::vector<std::string>* g_spineHrefSink;
extern size_t g_refuseNothrowArraysAbove;
extern size_t g_refusedNothrowArrays;
std::unordered_set<void*>& liveNothrowArrays();
}  // namespace opf_test_hooks

namespace {
struct ScopedSpineHrefSink {
  explicit ScopedSpineHrefSink(std::vector<std::string>* sink) { opf_test_hooks::g_spineHrefSink = sink; }
  ~ScopedSpineHrefSink() { opf_test_hooks::g_spineHrefSink = nullptr; }
};

// Makes the heap refuse any block larger than the item store's 4 KB read/write buffers, as a device
// short of a large block would: the spine idref table then gets what fits (a few hundred entries a
// pass), and the store stays buffered.
struct ScopedIndexGrowthOom {
  ScopedIndexGrowthOom() {
    opf_test_hooks::g_refusedNothrowArrays = 0;
    opf_test_hooks::g_refuseNothrowArraysAbove = std::max(serialization::BufferedFileReader::DEFAULT_BUFFER_BYTES,
                                                          serialization::BufferedFileWriter::DEFAULT_BUFFER_BYTES);
  }
  ~ScopedIndexGrowthOom() { opf_test_hooks::g_refuseNothrowArraysAbove = 0; }
};
}  // namespace

TEST(ContentOpfParser, ExtractsMetadataManifestAndGuideFields) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>"
      "<dc:title>Main Title</dc:title>"
      "<dc:title>Ignored Subtitle</dc:title>"
      "<dc:creator>Author One</dc:creator>"
      "<dc:creator>Author Two</dc:creator>"
      "<dc:language>en</dc:language>"
      "<dc:description>&lt;p&gt; Hello &lt;b&gt;World&lt;/b&gt; &lt;/p&gt;</dc:description>"
      "<meta name='cover' content='cover-xhtml'/>"
      "<meta name='calibre:series' content='Series Name'/>"
      "<meta name='calibre:series_index' content='2'/>"
      "</metadata>"
      "<manifest>"
      "<item id='cover-xhtml' href='text/cover.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='cover-image' href='images/cover.jpg' media-type='image/jpeg' properties='cover-image'/>"
      "<item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
      "<item id='nav' href='toc-nav.xhtml' media-type='application/xhtml+xml' properties='nav'/>"
      "<item id='css' href='styles/main.css' media-type='text/css'/>"
      "<item id='pagemap' href='page-map.xml' media-type='application/oebps-page-map+xml'/>"
      "</manifest>"
      "<guide>"
      "<reference type='text' href='text/chapter-1.xhtml'/>"
      "<reference type='cover' href='text/cover.xhtml'/>"
      "</guide>"
      "</package>";

  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  EXPECT_EQ(parser.title, "Main Title");
  EXPECT_EQ(parser.author, "Author One, Author Two");
  EXPECT_EQ(parser.primaryAuthor, "Author One");
  EXPECT_EQ(parser.language, "en");
  EXPECT_EQ(parser.description, "Hello World");
  EXPECT_EQ(parser.series, "Series Name");
  EXPECT_EQ(parser.seriesIndex, "2");

  EXPECT_EQ(parser.tocNcxPath, "book/OEBPS/toc.ncx");
  EXPECT_EQ(parser.tocNavPath, "book/OEBPS/toc-nav.xhtml");
  EXPECT_EQ(parser.pageMapPath, "book/OEBPS/page-map.xml");
  ASSERT_EQ(parser.cssFiles.size(), 1u);
  EXPECT_EQ(parser.cssFiles[0], "book/OEBPS/styles/main.css");

  // meta cover points to XHTML wrapper, so it should be ignored in favor of
  // EPUB3 properties="cover-image" image item.
  EXPECT_EQ(parser.coverItemHref, "book/OEBPS/images/cover.jpg");
  EXPECT_EQ(parser.textReferenceHref, "book/OEBPS/text/chapter-1.xhtml");
  EXPECT_EQ(parser.guideCoverPageHref, "book/OEBPS/text/cover.xhtml");
}

// A cover/metadata load (Epub::loadForCover / loadForMetadata) passes a null cache. It must still
// resolve the EPUB2 <meta name="cover"> -> manifest image item, and it must NOT build the .items.bin
// item store: that store exists only to resolve spine idrefs, which a null cache skips entirely.
// Building it there wrote the whole manifest to SD per cover pass and — since those load paths never
// call setupCacheDir() — usually failed to open and logged three "probably fatal" errors per book.
TEST(ContentOpfParser, ResolvesMetaCoverWithoutCacheAndWritesNoItemStore) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>"
      "<dc:title>Cover By Meta</dc:title>"
      "<meta name='cover' content='cover-img'/>"
      "</metadata>"
      "<manifest>"
      "<item id='chap1' href='text/chapter-1.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='cover-img' href='images/front.jpeg' media-type='image/jpeg'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='chap1'/>"
      "</spine>"
      "</package>";

  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  // The cover href is matched inline as manifest items stream past — never read back from the
  // item store — so dropping the store cannot break it.
  EXPECT_EQ(parser.coverItemHref, "book/OEBPS/images/front.jpeg");
  EXPECT_EQ(parser.title, "Cover By Meta");

  // The store must never have been created. (The destructor removes it, so check while the
  // parser is still alive.)
  EXPECT_FALSE(std::filesystem::exists(cacheDir + "/.items.bin"))
      << "null-cache parse built an item store it will never read";
}

TEST(ContentOpfParser, DecodesNumericCharacterReferencesInDescription) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  const std::string base = "/book/OEBPS/";
  // Calibre-style descriptions frequently double-escape the inner HTML, so the
  // parser sees the literal "&#8212;" after the outer &amp; is resolved. It must
  // decode both decimal (&#8212; → em dash) and hex (&#x2019; → right quote).
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>"
      "<dc:title>T</dc:title>"
      "<dc:description>A &amp;#8212; B&amp;#x2019;s tale</dc:description>"
      "</metadata>"
      "<manifest>"
      "<item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/>"
      "</manifest>"
      "<spine/>"
      "</package>";

  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  // U+2014 EM DASH = e2 80 94, U+2019 RIGHT SINGLE QUOTATION MARK = e2 80 99
  EXPECT_EQ(parser.description, "A \xE2\x80\x94 B\xE2\x80\x99s tale");
}

TEST(ContentOpfParser, ResolvesSpineIdrefsUsingManifestItems) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>"
      "<item id='ch1' href='text/ch1.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ch2' href='text/ch2.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ch2'/>"
      "<itemref idref='missing'/>"
      "<itemref idref='ch1'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch2.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch1.xhtml");
}

TEST(ContentOpfParser, OversizedFirstManifestIdNeitherHangsNorBreaksLaterIdrefs) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  // A 5000-char id on the first manifest item. SaxParserYxml truncates attribute
  // values to kAttrValueLen (384), so the item store records a 383-char id rather
  // than one past serialization's MAX_STRING_LENGTH. The spine pass must still
  // walk past that record: complete without looping, and resolve every later idref.
  const std::string oversizedId = repeatedChar('m', 5000);
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>"
      "<item id='" +
      oversizedId +
      "' href='text/bad.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ok1' href='text/ok1.xhtml' media-type='application/xhtml+xml'/>"
      "<item id='ok2' href='text/ok2.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ok1'/>"
      "<itemref idref='ok2'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ok1.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ok2.xhtml");
}

TEST(ContentOpfParser, ResolvesOutOfOrderSpineIdrefsInALargeManifest) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  constexpr int kItemCount = 420;
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "</manifest>"
      "<spine>"
      "<itemref idref='ch419'/>"
      "<itemref idref='ch10'/>"
      "<itemref idref='missing'/>"
      "<itemref idref='ch0'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 3u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch419.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch10.xhtml");
  EXPECT_EQ(capturedSpineHrefs[2], "book/OEBPS/text/ch0.xhtml");
}

// A huge manifest (the King's Avatar case: 1732 items) must parse without aborting and resolve every
// spine idref, whatever table the heap allows: the regression guard for "a huge book crashes the
// device" (an index once aborted on -fno-exceptions) and, with the order tests below, for "the
// fallback is O(N^2) slow".
namespace {
// Parses a 1500-item manifest whose spine names its last, a middle and its first item plus one
// unknown id, and checks the three known ones resolve in spine order.
void expectHugeManifestResolves() {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  constexpr int kItemCount = 1500;
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>Huge</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "</manifest>"
      "<spine>"
      "<itemref idref='ch1499'/>"  // last item — resolvable only if the whole manifest was stored
      "<itemref idref='ch750'/>"   // middle
      "<itemref idref='missing'/>"
      "<itemref idref='ch0'/>"  // first
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml)) << "a 1500-item manifest must parse without aborting";

  ASSERT_EQ(capturedSpineHrefs.size(), 3u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch1499.xhtml");
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch750.xhtml");
  EXPECT_EQ(capturedSpineHrefs[2], "book/OEBPS/text/ch0.xhtml");
}
}  // namespace

TEST(ContentOpfParser, HugeManifestResolvesThroughAFullTable) {
  // The host heap holds a table for every itemref: one pass over the item store.
  expectHugeManifestResolves();
}

TEST(ContentOpfParser, HugeManifestResolvesWhenTheHeapRefusesALargeTable) {
  // The refused sizes halve down to a table that fits; it resolves the same hrefs.
  const ScopedIndexGrowthOom oom;
  expectHugeManifestResolves();
  EXPECT_GT(opf_test_hooks::g_refusedNothrowArrays, 0u) << "no table was refused; the smaller one did not run";
}

// Every idref used to rescan .items.bin from its first record: a 2956-itemref book read ~4.4
// million records and spent 75 s in this parse on the device. Spine order follows manifest order
// in practice, so a pass that resumes after the previous table-full's matches, and writes each
// itemref out as soon as it is the next one due, reads the store about once. Measured in bytes
// read, not time: a host finishes either way quickly, the device does not.
TEST(ContentOpfParser, AnInOrderSpineReadsTheItemStoreAboutOnceOnASmallHeap) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  constexpr int kItemCount = 3000;
  std::string spine;
  for (int i = 0; i < kItemCount; ++i) spine += "<itemref idref='ch" + std::to_string(i) + "'/>";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>Long</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) + "</manifest><spine>" + spine + "</spine></package>";

  const ScopedIndexGrowthOom oom;
  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, "/book/OEBPS/", xml.size(), &cache);
  const size_t bytesReadBefore = FsFile::bytesRead;
  ASSERT_TRUE(parseOpfXml(parser, xml));
  const size_t bytesRead = FsFile::bytesRead - bytesReadBefore;

  EXPECT_GT(opf_test_hooks::g_refusedNothrowArrays, 0u) << "no table was refused; the small heap did not apply";
  ASSERT_EQ(capturedSpineHrefs.size(), static_cast<size_t>(kItemCount));
  for (int i = 0; i < kItemCount; ++i) {
    ASSERT_EQ(capturedSpineHrefs[i], "book/OEBPS/text/ch" + std::to_string(i) + ".xhtml") << "itemref " << i;
  }
  const auto storeBytes = static_cast<size_t>(std::filesystem::file_size(cacheDir + "/.items.bin"));
  EXPECT_LE(bytesRead, 2 * storeBytes) << "item store is " << storeBytes << " bytes";
}

// A spine whose order the manifest does not follow. Resuming after the previous match reads the
// store about once for a spine in manifest order, and about three times for a manifest in name
// order (Sigil's: ch0, ch1, ch10, ch100, ...); a manifest in reverse spine order takes it a whole
// store per itemref -- quadratic in the spine, the scan the X3 fell back to when King's Avatar's
// manifest index could not grow past 1024 items (2026-10-20). A table-full of itemrefs per pass
// reads it a few times in all, in either order, on the same small heap.
namespace {
double itemStoreReadsFor(const std::vector<int>& manifestOrder, const int images) {
  const std::string cacheDir = makeTempDir();
  EXPECT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);
  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  const int chapters = static_cast<int>(manifestOrder.size());
  std::string manifest;
  for (const int i : manifestOrder) {
    manifest += "<item id='ch" + std::to_string(i) + "' href='text/ch" + std::to_string(i) +
                ".xhtml' media-type='application/xhtml+xml'/>";
  }
  for (int i = 0; i < images; ++i) {
    manifest += "<item id='img" + std::to_string(i) + "' href='images/img" + std::to_string(i) +
                ".jpg' media-type='image/jpeg'/>";
  }
  std::string spine;
  for (int i = 0; i < chapters; ++i) spine += "<itemref idref='ch" + std::to_string(i) + "'/>";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>Order</dc:title></metadata>"
      "<manifest>" +
      manifest + "</manifest><spine>" + spine + "</spine></package>";

  const ScopedIndexGrowthOom oom;
  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, "/book/OEBPS/", xml.size(), &cache);
  const size_t bytesReadBefore = FsFile::bytesRead;
  EXPECT_TRUE(parseOpfXml(parser, xml));
  const size_t bytesRead = FsFile::bytesRead - bytesReadBefore;

  EXPECT_EQ(capturedSpineHrefs.size(), static_cast<size_t>(chapters));
  for (int i = 0; i < chapters && i < static_cast<int>(capturedSpineHrefs.size()); ++i) {
    const std::string want = "book/OEBPS/text/ch" + std::to_string(i) + ".xhtml";
    EXPECT_EQ(capturedSpineHrefs[i], want) << "itemref " << i;
    if (capturedSpineHrefs[i] != want) break;
  }
  const auto storeBytes = static_cast<size_t>(std::filesystem::file_size(cacheDir + "/.items.bin"));
  return static_cast<double>(bytesRead) / static_cast<double>(storeBytes);
}
}  // namespace

TEST(ContentOpfParser, ASpineInAnotherOrderThanTheManifestReadsTheItemStoreAFewTimesOnASmallHeap) {
  constexpr int kChapters = 1732;
  constexpr int kImages = 1600;
  std::vector<int> nameOrder(kChapters);
  for (int i = 0; i < kChapters; ++i) nameOrder[i] = i;
  std::sort(nameOrder.begin(), nameOrder.end(),
            [](const int a, const int b) { return std::to_string(a) < std::to_string(b); });
  std::vector<int> reverseOrder(kChapters);
  for (int i = 0; i < kChapters; ++i) reverseOrder[i] = kChapters - 1 - i;

  const double byName = itemStoreReadsFor(nameOrder, kImages);
  const double reversed = itemStoreReadsFor(reverseOrder, kImages);
  std::printf(
      "[ opf      ] %d itemrefs, 4 KB blocks: item store read %.1fx with the manifest in name order, %.1fx in "
      "reverse\n",
      kChapters, byName, reversed);
  // A 4 KB table holds 256 itemrefs: seven passes, and the hrefs found out of turn read back one by one.
  EXPECT_LE(byName, 12.0);
  EXPECT_LE(reversed, 12.0) << "the resumed scan of old read the store once per itemref here";
}

TEST(ContentOpfParser, ARepeatedManifestIdResolvesToItsFirstItem) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);

  std::vector<std::string> capturedSpineHrefs;
  ScopedSpineHrefSink sinkGuard(&capturedSpineHrefs);

  // Two manifest items share an id (invalid EPUB): the ambiguous idref resolves to its FIRST
  // manifest occurrence, and unrelated idrefs keep resolving normally.
  constexpr int kItemCount = 420;
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:title>T</dc:title></metadata>"
      "<manifest>" +
      buildManifestItems(kItemCount) +
      "<item id='ch5' href='text/duplicate.xhtml' media-type='application/xhtml+xml'/>"
      "</manifest>"
      "<spine>"
      "<itemref idref='ch5'/>"
      "<itemref idref='ch419'/>"
      "</spine>"
      "</package>";

  BookMetadataCache cache(cacheDir);
  ContentOpfParser parser(cacheDir, base, xml.size(), &cache);
  ASSERT_TRUE(parseOpfXml(parser, xml));

  ASSERT_EQ(capturedSpineHrefs.size(), 2u);
  EXPECT_EQ(capturedSpineHrefs[0], "book/OEBPS/text/ch5.xhtml");  // first occurrence wins
  EXPECT_EQ(capturedSpineHrefs[1], "book/OEBPS/text/ch419.xhtml");
}

namespace {

// What the creator handling produced for one <metadata> block.
struct CreatorResult {
  bool parsed = false;
  std::string author;
  std::string primaryAuthor;
  std::string authorSort;
  std::string series;
  std::string seriesIndex;
  std::string isbn;
  std::string asin;
  std::string uniqueIdentifier;
  std::string uuidIdentifier;
};

// Parses an OPF whose <metadata> holds `metadata`, fed `chunk` bytes per write() (0 = all at once),
// so a test can make the parser receive one creator's text in many pieces. `packageAttributes` go on
// the <package> element.
CreatorResult parseCreators(const std::string& metadata, const size_t chunk = 0,
                            const std::string& packageAttributes = "") {
  CreatorResult result;
  const std::string cacheDir = makeTempDir();
  if (cacheDir.empty()) return result;
  TempDirGuard dirGuard(cacheDir);
  const std::string base = "/book/OEBPS/";
  const std::string xml =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package " +
      packageAttributes +
      " xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata>" +
      metadata +
      "</metadata>"
      "<manifest><item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/></manifest>"
      "<spine/>"
      "</package>";
  ContentOpfParser parser(cacheDir, base, xml.size(), nullptr);
  if (!parser.setup()) return result;
  const auto* data = reinterpret_cast<const uint8_t*>(xml.data());
  const size_t step = chunk == 0 ? xml.size() : chunk;
  for (size_t at = 0; at < xml.size(); at += step) {
    const size_t n = std::min(step, xml.size() - at);
    if (parser.write(data + at, n) != n) return result;
  }
  result.parsed = true;
  result.author = parser.author;
  result.primaryAuthor = parser.primaryAuthor;
  result.authorSort = parser.authorSort;
  result.series = parser.series;
  result.seriesIndex = parser.seriesIndex;
  result.isbn = parser.isbn;
  result.asin = parser.asin;
  result.uniqueIdentifier = parser.uniqueIdentifier;
  result.uuidIdentifier = parser.uuidIdentifier;
  return result;
}

}  // namespace

// The display line lists every creator; the Library groups by the first one credited as author.
TEST(ContentOpfParserCreators, JoinsEveryCreatorAndPicksTheFirstAuthorAsPrimary) {
  const auto r = parseCreators(
      "<dc:creator opf:role='trl'>Anthea Bell</dc:creator>"
      "<dc:creator opf:role='aut' opf:file-as='Pratchett, Terry'>Terry Pratchett</dc:creator>"
      "<dc:creator opf:role='aut'>Neil Gaiman</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Anthea Bell, Terry Pratchett, Neil Gaiman");
  EXPECT_EQ(r.primaryAuthor, "Terry Pratchett");
  EXPECT_EQ(r.authorSort, "Pratchett, Terry");
}

TEST(ContentOpfParserCreators, ACreatorWithNoRoleIsAnAuthor) {
  const auto r = parseCreators("<dc:creator>\n  Ursula K. Le Guin \n</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Ursula K. Le Guin");
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "");
}

TEST(ContentOpfParserCreators, TheRoleCodeIsMatchedWhateverItsCase) {
  const auto r =
      parseCreators("<dc:creator opf:role='edt'>Ed Itor</dc:creator><dc:creator opf:role='AUT'>Au Thor</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Au Thor");
}

TEST(ContentOpfParserCreators, NoCreatorCreditedAsAuthorLeavesNoPrimaryAuthor) {
  const auto r = parseCreators("<dc:creator opf:role='edt'>Ed Itor</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Ed Itor");
  EXPECT_EQ(r.primaryAuthor, "");
  EXPECT_EQ(r.authorSort, "");
}

// EPUB 3 states a creator's role and filing name in <meta refines="#id">, after the creator.
TEST(ContentOpfParserCreators, Epub3RefinementsGiveRoleAndFileAs) {
  const auto r = parseCreators(
      "<dc:creator id='ill'>Pauline Baynes</dc:creator>"
      "<dc:creator id='c2'>Ursula K. Le Guin</dc:creator>"
      "<meta refines='#ill' property='role' scheme='marc:relators'>ill</meta>"
      "<meta refines='#c2' property='role' scheme='marc:relators'>aut</meta>"
      "<meta refines='#c2' property='file-as'> Le Guin, Ursula K. </meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Pauline Baynes, Ursula K. Le Guin");
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "Le Guin, Ursula K.");
}

// A refinement of something that is not a creator is not ours to take.
TEST(ContentOpfParserCreators, RefinementsOfOtherThingsLeaveCreatorsAlone) {
  const auto r = parseCreators(
      "<dc:title id='t'>Tehanu</dc:title>"
      "<dc:creator id='c1'>Ursula K. Le Guin</dc:creator>"
      "<meta refines='#t' property='file-as'>Tehanu, The Last Book</meta>"
      "<meta refines='#nobody' property='role'>edt</meta>"
      "<meta property='belongs-to-collection' id='coll'>Earthsea</meta>"
      "<meta refines='#coll' property='group-position'>4</meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(r.authorSort, "");
  EXPECT_EQ(r.series, "Earthsea");
  EXPECT_EQ(r.seriesIndex, "4");
}

// One creator's text may reach the parser in several pieces (a write boundary, an entity). The
// separator belongs between creators, never inside one.
TEST(ContentOpfParserCreators, ACreatorFedOneByteAtATimeStaysOneName) {
  const auto r = parseCreators("<dc:creator>Laurel &amp; Hardy</dc:creator><dc:creator>Second</dc:creator>", 1);
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Laurel & Hardy, Second");
  EXPECT_EQ(r.primaryAuthor, "Laurel & Hardy");
}

// A creator written surname-first is still one creator.
TEST(ContentOpfParserCreators, ASurnameFirstCreatorStaysOneName) {
  const auto r = parseCreators("<dc:creator>Le Guin, Ursula K.</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Le Guin, Ursula K.");
  EXPECT_EQ(r.primaryAuthor, "Le Guin, Ursula K.");
}

// Only the first MAX_CREATORS (4) are candidates for primary author, but the
// display line keeps them all.
TEST(ContentOpfParserCreators, ManyCreatorsAllReachTheDisplayLine) {
  const auto r = parseCreators(
      "<dc:creator opf:role='ill'>I1</dc:creator><dc:creator opf:role='ill'>I2</dc:creator>"
      "<dc:creator opf:role='ill'>I3</dc:creator><dc:creator opf:role='ill'>I4</dc:creator>"
      "<dc:creator opf:role='aut'>A5</dc:creator><dc:creator>A6</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "I1, I2, I3, I4, A5, A6");
  EXPECT_EQ(r.primaryAuthor, "");
}

// EPUB 3 lets a creator hold several roles. Being credited as author is what counts: an author who
// also illustrated stays the primary author whichever role is stated last.
TEST(ContentOpfParserCreators, AnAuthorWithASecondRoleStaysTheAuthor) {
  const auto r = parseCreators(
      "<dc:creator id='c1'>Maurice Sendak</dc:creator>"
      "<meta refines='#c1' property='role' scheme='marc:relators'>aut</meta>"
      "<meta refines='#c1' property='role' scheme='marc:relators'>ill</meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Maurice Sendak");
}

TEST(ContentOpfParserCreators, AnOpfAuthorRoleSurvivesALaterRefinedRole) {
  const auto r = parseCreators(
      "<dc:creator id='c1' opf:role='aut'>Maurice Sendak</dc:creator>"
      "<meta refines='#c1' property='role' scheme='marc:relators'>ill</meta>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.primaryAuthor, "Maurice Sendak");
}

// The creator table serves only to pick the primary author when <metadata> closes. It must be gone
// by then: a first open goes on through the manifest and spine with this parser alive, and a large
// book needs every byte of heap there.
TEST(ContentOpfParserCreators, TheCreatorTableIsFreedWhenMetadataCloses) {
  const std::string cacheDir = makeTempDir();
  ASSERT_FALSE(cacheDir.empty());
  TempDirGuard dirGuard(cacheDir);
  const std::string head =
      "<?xml version='1.0' encoding='utf-8'?>"
      "<package xmlns:opf='http://www.idpf.org/2007/opf' xmlns:dc='http://purl.org/dc/elements/1.1/'>"
      "<metadata><dc:creator>Ursula K. Le Guin</dc:creator></metadata>";
  const std::string rest =
      "<manifest><item id='ncx' href='toc.ncx' media-type='application/x-dtbncx+xml'/></manifest>"
      "<spine/></package>";
  ContentOpfParser parser(cacheDir, "/book/OEBPS/", head.size() + rest.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  const size_t before = opf_test_hooks::liveNothrowArrays().size();
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(head.data()), head.size()), head.size());
  EXPECT_EQ(parser.primaryAuthor, "Ursula K. Le Guin");
  EXPECT_EQ(opf_test_hooks::liveNothrowArrays().size(), before) << "the creator table outlived </metadata>";
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(rest.data()), rest.size()), rest.size());
}

// An empty creator adds nothing, not even a separator.
TEST(ContentOpfParserCreators, AWhitespaceOnlyCreatorIsSkipped) {
  const auto r = parseCreators("<dc:creator> </dc:creator><dc:creator>Real Name</dc:creator>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.author, "Real Name");
  EXPECT_EQ(r.primaryAuthor, "Real Name");
}

// ISBN and ASIN, for KOReader sync's metadata. Which dc:identifier is which follows crosspoint-reader
// PR #3804 (Jadehawk / @jadehawk): the scheme names it, or the value's own prefix does.
TEST(ContentOpfParserIdentifiers, AnIsbnSchemeNamesTheIsbn) {
  const auto r = parseCreators("<dc:identifier opf:scheme='ISBN'>9780441013593</dc:identifier>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.isbn, "9780441013593");
  EXPECT_EQ(r.asin, "");
}

TEST(ContentOpfParserIdentifiers, AnIsbnPrefixIsDropped) {
  EXPECT_EQ(parseCreators("<dc:identifier id='pub-id'>urn:isbn:978-0-441-01359-3</dc:identifier>").isbn,
            "978-0-441-01359-3");
  EXPECT_EQ(parseCreators("<dc:identifier>ISBN: 9780441013593</dc:identifier>").isbn, "9780441013593");
}

// Calibre writes an Amazon identifier with the scheme MOBI-ASIN or AMAZON.
TEST(ContentOpfParserIdentifiers, AsinSchemesAndPrefixesNameTheAsin) {
  EXPECT_EQ(parseCreators("<dc:identifier opf:scheme='MOBI-ASIN'>B000FC1PJI</dc:identifier>").asin, "B000FC1PJI");
  EXPECT_EQ(parseCreators("<dc:identifier opf:scheme='AMAZON'>B000FC1PJI</dc:identifier>").asin, "B000FC1PJI");
  EXPECT_EQ(parseCreators("<dc:identifier>urn:asin:B000FC1PJI</dc:identifier>").asin, "B000FC1PJI");
}

TEST(ContentOpfParserIdentifiers, OtherIdentifiersAreNeither) {
  const auto r = parseCreators(
      "<dc:identifier opf:scheme='calibre'>0f3c5e1a-6b0e-4a4e-9c1d-1d2a3b4c5d6e</dc:identifier>"
      "<dc:identifier id='uuid_id' opf:scheme='uuid'>urn:uuid:1234</dc:identifier>");
  ASSERT_TRUE(r.parsed);
  EXPECT_EQ(r.isbn, "");
  EXPECT_EQ(r.asin, "");
}

TEST(ContentOpfParserIdentifiers, TheFirstOfEachIsKept) {
  const auto r = parseCreators(
      "<dc:identifier opf:scheme='ISBN'>9780441013593</dc:identifier>"
      "<dc:identifier opf:scheme='AMAZON'>B000FC1PJI</dc:identifier>"
      "<dc:identifier opf:scheme='ISBN'>9780000000000</dc:identifier>"
      "<dc:identifier opf:scheme='AMAZON'>B999999999</dc:identifier>");
  EXPECT_EQ(r.isbn, "9780441013593");
  EXPECT_EQ(r.asin, "B000FC1PJI");
}

TEST(ContentOpfParserIdentifiers, AnIdentifierFedOneByteAtATimeStaysWholeAndTrimmed) {
  EXPECT_EQ(parseCreators("<dc:identifier>\n  urn:isbn:9780441013593\n</dc:identifier>", 1).isbn, "9780441013593");
}

// No ISBN is this long, and no identifier at all is longer than 256 bytes; a cut-off one would be a
// wrong one, so it is not kept at all.
TEST(ContentOpfParserIdentifiers, AnOverlongIdentifierIsNotKept) {
  const std::string digits(200, '9');
  const auto r = parseCreators("<dc:identifier opf:scheme='ISBN'>" + digits + "</dc:identifier>");
  EXPECT_EQ(r.isbn, "");
  EXPECT_EQ(r.uniqueIdentifier, digits);  // still the package's identifier
  EXPECT_EQ(parseCreators("<dc:identifier>" + std::string(257, 'x') + "</dc:identifier>").uniqueIdentifier, "");
}

// The keys of a book's obfuscated fonts (BookFonts): the identifier the package's unique-identifier
// names, else the first, and the first urn:uuid: one.
TEST(ContentOpfParserIdentifiers, TheUniqueIdentifierIsTheOneThePackageNames) {
  const std::string identifiers =
      "<dc:identifier id='isbn'>urn:isbn:9780441013593</dc:identifier>"
      "<dc:identifier id='uuid' opf:scheme='uuid'>\n URN:UUID:0123-4567 </dc:identifier>"
      "<dc:identifier id='other'>urn:uuid:89ab</dc:identifier>";
  const auto named = parseCreators(identifiers, 0, "unique-identifier='uuid'");
  EXPECT_EQ(named.uniqueIdentifier, "URN:UUID:0123-4567");
  EXPECT_EQ(named.uuidIdentifier, "URN:UUID:0123-4567");
  EXPECT_EQ(named.isbn, "9780441013593");
  EXPECT_EQ(parseCreators(identifiers, 7, "unique-identifier='other'").uniqueIdentifier, "urn:uuid:89ab");
  const auto unnamed = parseCreators(identifiers, 0, "unique-identifier='missing'");
  EXPECT_EQ(unnamed.uniqueIdentifier, "urn:isbn:9780441013593");  // the first
  EXPECT_EQ(unnamed.uuidIdentifier, "URN:UUID:0123-4567");
  EXPECT_EQ(parseCreators("<dc:title>T</dc:title>").uniqueIdentifier, "");
}

// A read that wants the metadata alone stops the stream once </metadata> has closed.
TEST(ContentOpfParserIdentifiers, MetadataReadIsSetWhenTheMetadataCloses) {
  const std::string head =
      "<?xml version='1.0'?><package xmlns:dc='http://purl.org/dc/elements/1.1/'><metadata>"
      "<dc:identifier>urn:uuid:01</dc:identifier>";
  const std::string rest = "</metadata><manifest/><spine/></package>";
  const std::string noCache;
  ContentOpfParser parser(noCache, noCache, head.size() + rest.size(), nullptr);
  ASSERT_TRUE(parser.setup());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(head.data()), head.size()), head.size());
  EXPECT_FALSE(*parser.metadataRead());
  ASSERT_EQ(parser.write(reinterpret_cast<const uint8_t*>(rest.data()), rest.size()), rest.size());
  EXPECT_TRUE(*parser.metadataRead());
  EXPECT_EQ(parser.uuidIdentifier, "urn:uuid:01");
}
