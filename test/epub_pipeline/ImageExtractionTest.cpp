// Lazy image extraction (ImageBlock::ensureExtracted -> Epub::extractItemToFile).
//
// The inflate ring this path needs is up to 32 KB CONTIGUOUS, and until it could be served
// from an arena it was the first allocation to fail on a fragmented heap: measured on X4 at
// contig=13300, every image on the page logged "Failed to init inflate reader" and rendered
// as nothing at all. The reader is already holding a borrowed 48 KB arena for the decoders at
// that moment (image_scratch), so the fix is to hand it to the extractor too.
//
// What has to hold:
//   1. The arena path produces exactly the same bytes as the heap path.
//   2. The ring really comes from the arena, and is given back afterwards.
//   3. An arena too small to serve the reader falls back to the heap instead of failing —
//      EntryReader itself does NOT fall back once it has been handed one.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "BuildArena.h"
#include "Epub.h"
#include "Epub/Section.h"
#include "Epub/blocks/ImageBlock.h"
#include "GfxRenderer.h"
#include "StoredZipWriter.h"

namespace fs = std::filesystem;

namespace {

const char* kBook = CORPUS_DIR "/test_png_images.epub";
// 36353 bytes uncompressed, so ringSizeFor() asks for the full 32 KB cap — the worst case,
// and the one the device actually failed on.
const char* kEntry = "OEBPS/images/scaling_test.png";
constexpr size_t kEntryBytes = 36353;

struct ImageExtractionFixture : testing::Test {
  fs::path work;
  std::string cacheDir;

  void SetUp() override {
    // Per-test dir: ctest -j runs these as parallel processes.
    work = fs::temp_directory_path() /
           (std::string("epub_extract_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
  }
  void TearDown() override { fs::remove_all(work); }

  std::vector<uint8_t> extract(const char* name, BuildArena* arena) {
    const std::string dest = (work / name).string();
    Epub epub(kBook, cacheDir);
    if (!epub.extractItemToFile(kEntry, dest, arena)) return {};
    std::ifstream in(dest, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

// The write buffer is reserved from the arena BEFORE the reader takes its block, because
// BuildArena is LIFO. Get that order wrong and the release is rejected, leaking the block for
// the rest of the pass — so assert the arena comes back empty, not just that the bytes are right.
TEST_F(ImageExtractionFixture, ArenaPathMatchesHeapByteForByte) {
  const auto viaHeap = extract("heap.png", nullptr);
  ASSERT_EQ(viaHeap.size(), kEntryBytes);
  EXPECT_EQ(viaHeap[0], 0x89);  // PNG signature survived the round trip
  EXPECT_EQ(viaHeap[1], 'P');

  BuildArena arena(Epub::EXTRACT_ARENA_BYTES + 1024);  // budget + alignment slack
  ASSERT_TRUE(arena.valid());
  const auto viaArena = extract("arena.png", &arena);

  EXPECT_EQ(viaArena, viaHeap);
  EXPECT_GT(arena.highWater(), 32u * 1024u + Epub::EXTRACT_WRITE_BUFFER_BYTES)
      << "both the ring and the write buffer should have come from the arena";
  EXPECT_EQ(arena.used(), 0u) << "write buffer and reader block must both be released";
  EXPECT_EQ(arena.failedAllocSize(), 0u);
}

// EXTRACT_ARENA_BYTES is what callers gate on; if it were too small for the reader, every
// extraction would silently take the slow fallback and the fix would do nothing.
TEST_F(ImageExtractionFixture, BudgetConstantActuallyCoversTheReader) {
  BuildArena arena(Epub::EXTRACT_ARENA_BYTES);
  ASSERT_TRUE(arena.valid());
  const auto bytes = extract("budget.png", &arena);

  ASSERT_EQ(bytes.size(), kEntryBytes);
  EXPECT_EQ(arena.failedAllocSize(), 0u) << "EXTRACT_ARENA_BYTES must fit readBuf + ring + alignment";
}

TEST_F(ImageExtractionFixture, TooSmallArenaFallsBackToHeapInsteadOfFailing) {
  const auto viaHeap = extract("heap.png", nullptr);
  ASSERT_EQ(viaHeap.size(), kEntryBytes);

  BuildArena arena(2 * 1024);  // fits the read buffer, nowhere near the ring
  ASSERT_TRUE(arena.valid());
  const auto bytes = extract("small.png", &arena);

  EXPECT_EQ(bytes, viaHeap) << "a short arena must not turn a working extraction into a failure";
  EXPECT_GT(arena.failedAllocSize(), 0u) << "the arena path should have been tried first";
  EXPECT_EQ(arena.used(), 0u) << "the failed open must release its scope";
}

}  // namespace

// An arena with room for the reader but not the write buffer must still extract correctly — the
// buffer falls back to the heap, and then to unbuffered pass-through. Slow is a nuisance; a
// truncated image is a bug.
TEST_F(ImageExtractionFixture, WriteBufferFallsBackWithoutBreakingTheExtract) {
  const auto viaHeap = extract("reference.png", nullptr);
  ASSERT_EQ(viaHeap.size(), kEntryBytes);

  BuildArena arena(33 * 1024 + 1024);  // reader fits, write buffer does not
  ASSERT_TRUE(arena.valid());
  const auto bytes = extract("tight.png", &arena);

  EXPECT_EQ(bytes, viaHeap);
  EXPECT_EQ(arena.used(), 0u);
}

// --- extracts that are not the entry ------------------------------------------------------
//
// An extract is written under a temporary name and renamed once complete, and a render checks
// an existing extract's size against the entry before decoding it. Both exist because a reset
// mid-extract left a short JPEG on the card that every later visit decoded as "no SOF marker"
// (X3 2026-09-25): the file existed, so nothing ever re-extracted it.

TEST_F(ImageExtractionFixture, AFailedExtractLeavesNothingBehind) {
  const std::string dest = (work / "absent.png").string();
  Epub epub(kBook, cacheDir);
  EXPECT_FALSE(epub.extractItemToFile("OEBPS/images/absent.png", dest, nullptr));
  EXPECT_FALSE(fs::exists(dest));
  EXPECT_FALSE(fs::exists(dest + ".part")) << "the temporary name must not survive a failure either";
}

TEST_F(ImageExtractionFixture, ASuccessfulExtractLeavesOnlyTheFinalName) {
  const std::string dest = (work / "whole.png").string();
  Epub epub(kBook, cacheDir);
  ASSERT_TRUE(epub.extractItemToFile(kEntry, dest, nullptr));
  EXPECT_EQ(fs::file_size(dest), kEntryBytes);
  EXPECT_FALSE(fs::exists(dest + ".part"));
}

TEST_F(ImageExtractionFixture, ATruncatedExtractIsReplacedBeforeItIsDecoded) {
  const std::string path = (work / "cut.png").string();
  {
    Epub epub(kBook, cacheDir);
    ASSERT_TRUE(epub.extractItemToFile(kEntry, path, nullptr));
  }
  ASSERT_EQ(fs::file_size(path), kEntryBytes);
  ImageBlock block(path, 120, 160, "", kBook, kEntry);
  fs::resize_file(path, 1000);  // what a reset mid-write used to leave behind
  ASSERT_EQ(fs::file_size(path), 1000u);

  GfxRenderer renderer;
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  EXPECT_EQ(fs::file_size(path), kEntryBytes) << "the short extract must be replaced by the whole entry";
  // Decoded from the fresh extract: the pixel cache exists, so the page will show the image.
  EXPECT_TRUE(fs::exists(path + ".1bit.pxc") || fs::exists((work / "cut.1bit.pxc").string()));
}

// --- the bytes pick the decoder, not the name ---------------------------------------------
//
// FB2 conversions reference their images with no extension at all (images/img_32, which the
// parser caches as .img), and some books call a PNG .jpg. A decoder chosen by the cache file's
// extension was then none, or the wrong one, and the page showed nothing where the picture was.

namespace {
const char* kJpegBook = CORPUS_DIR "/test_jpeg_images.epub";
const char* kJpegEntry = "OEBPS/images/jpeg_format.jpg";

// Renders a block whose cache file `name` is extracted from `entry` on first render, and reports
// whether the decode produced the 1-bit pixel cache that every later view of the page draws from.
bool decodesTo(const fs::path& work, const char* name, const char* book, const char* entry) {
  const fs::path path = work / name;
  ImageBlock block(path.string(), 120, 160, "", book, entry);
  GfxRenderer renderer;
  block.render(renderer, 0, 0, /*forceLoad=*/true, /*monochromeOutput=*/true);
  return fs::exists(fs::path(path).replace_extension(".1bit.pxc"));
}
}  // namespace

TEST_F(ImageExtractionFixture, APngNamedJpgDecodesAsAPng) {
  EXPECT_TRUE(decodesTo(work, "mislabeled.jpg", kBook, kEntry));
}

TEST_F(ImageExtractionFixture, AJpegNamedPngDecodesAsAJpeg) {
  EXPECT_TRUE(decodesTo(work, "mislabeled.png", kJpegBook, kJpegEntry));
}

TEST_F(ImageExtractionFixture, AnImageCachedWithoutItsExtensionDecodesByItsBytes) {
  EXPECT_TRUE(decodesTo(work, "nameless_png.img", kBook, kEntry));
  EXPECT_TRUE(decodesTo(work, "nameless_jpeg.img", kJpegBook, kJpegEntry));
}

// --- large-image placeholder gate ------------------------------------------------------------
//
// This used to compare ImageBlock's width*height — the DISPLAY dimensions — against 800*600.
// The panel is 480x800, and render() rejects anything larger than the screen, so the product
// could never exceed 384000 and the gate was unreachable: the "show a placeholder for large
// images" setting did nothing at all, for any book. The tests below pin both halves of the fix —
// that the decision is made on source bytes, and that display size does not enter into it.

struct LargeImageFixture : ImageExtractionFixture {
  // A file of `bytes` at the block's imagePath, standing in for an already-extracted image.
  std::string writeExtracted(const char* name, const size_t bytes) {
    const std::string path = (work / name).string();
    std::ofstream out(path, std::ios::binary);
    const std::vector<char> chunk(1024, '\0');
    for (size_t written = 0; written < bytes; written += chunk.size()) {
      out.write(chunk.data(), static_cast<std::streamsize>(std::min(chunk.size(), bytes - written)));
    }
    return path;
  }
};

TEST_F(LargeImageFixture, DecidesOnSourceBytesNotDisplaySize) {
  // Tiny on screen, huge on disk: large. This is the case that matters — a full-page cover is
  // scaled down to fit, so its display size says nothing about what it costs to get there.
  const std::string big = writeExtracted("big.png", LARGE_IMAGE_SOURCE_BYTES + 1);
  ImageBlock smallOnScreen(big, 32, 32, "");
  EXPECT_TRUE(smallOnScreen.isLargeImage());

  // Full screen, small file: not large. Under the old pixel test this was the only shape that
  // could ever have qualified, and even it could not reach the threshold.
  const std::string small = writeExtracted("small.png", 8 * 1024);
  ImageBlock fullScreen(small, 480, 800, "");
  EXPECT_FALSE(fullScreen.isLargeImage());
}

TEST_F(LargeImageFixture, ResolvesSizeFromTheArchiveBeforeExtraction) {
  // Nothing extracted yet: the size has to come from the ZIP entry, which is the state the gate
  // is actually consulted in (first view, pixel-cache miss, image not yet on SD).
  const std::string notExtracted = (work / "never_written.png").string();
  ASSERT_FALSE(fs::exists(notExtracted));

  static_assert(kEntryBytes < LARGE_IMAGE_SOURCE_BYTES, "fixture entry must be under the threshold");
  ImageBlock block(notExtracted, 100, 100, "", kBook, kEntry);
  EXPECT_FALSE(block.isLargeImage()) << kEntryBytes << " bytes is under the threshold";
  EXPECT_FALSE(fs::exists(notExtracted)) << "asking the size must not extract anything";

  // The corpus has no entry above the threshold, so the archive path's TRUE case is not covered
  // here — a failed lookup and a small entry both answer false, and there is no way to tell them
  // apart from outside. What is pinned is that the lookup neither extracts nor throws.
}

TEST_F(LargeImageFixture, UnknownSizeRendersRatherThanHides) {
  // No archive reference and no file on disk: nothing can be learned. Fall to "not large", so the
  // image is attempted rather than replaced by a placeholder that would never resolve.
  ImageBlock orphan((work / "missing.png").string(), 400, 600, "");
  EXPECT_FALSE(orphan.isLargeImage());
}

TEST_F(LargeImageFixture, PlaceholderIsSuppressedByForceLoad) {
  const std::string big = writeExtracted("forced.png", LARGE_IMAGE_SOURCE_BYTES + 1);
  ImageBlock block(big, 400, 600, "");

  EXPECT_TRUE(block.wouldShowPlaceholder(/*forceLoad=*/false, /*monochromeOutput=*/true));
  EXPECT_FALSE(block.wouldShowPlaceholder(/*forceLoad=*/true, /*monochromeOutput=*/true))
      << "the user asked for it explicitly";
}

TEST_F(LargeImageFixture, PlaceholderIsSuppressedByThePixelCacheOfThatMode) {
  const std::string big = writeExtracted("cached.png", LARGE_IMAGE_SOURCE_BYTES + 1);
  ImageBlock block(big, 400, 600, "");
  {
    std::ofstream((work / "cached.1bit.pxc").string()) << 'x';
  }

  EXPECT_FALSE(block.wouldShowPlaceholder(/*forceLoad=*/false, /*monochromeOutput=*/true));
  EXPECT_TRUE(block.wouldShowPlaceholder(/*forceLoad=*/false, /*monochromeOutput=*/false))
      << "the greyscale variant has no cache yet";
}

// The size probe is the expensive half (a failed open plus a central-directory scan when the
// image was never extracted), so it must not run while the cache already answers. Observed
// through its memo: had the first call probed, it would have remembered "large", and the second
// call -- cache gone, file now small -- would wrongly answer with a placeholder.
TEST_F(LargeImageFixture, SizeIsNotProbedWhileTheCacheExists) {
  const std::string path = writeExtracted("probe.png", LARGE_IMAGE_SOURCE_BYTES + 1);
  const std::string cache = (work / "probe.1bit.pxc").string();
  {
    std::ofstream(cache) << 'x';
  }
  ImageBlock block(path, 400, 600, "");
  ASSERT_FALSE(block.wouldShowPlaceholder(/*forceLoad=*/false, /*monochromeOutput=*/true));

  fs::remove(cache);
  fs::resize_file(path, 1024);
  EXPECT_FALSE(block.wouldShowPlaceholder(/*forceLoad=*/false, /*monochromeOutput=*/true))
      << "the size was probed (and remembered) while the cache existed";
}

// --- heap-degraded image headers -------------------------------------------------------------
//
// When neither the img tag nor the manifest can supply an image's dimensions, the parser reads
// the header straight out of the ZIP — behind a heap gate. A refusal drops the image to alt text
// and the section is then CACHED that way, under the same property hash a complete build would
// use, so the missing image is permanent. Background-B is the caller most exposed to it: it
// builds with the framebuffer borrowed, which is exactly when the largest free block is small.
//
// The distinction that matters is that the same VISIBLE outcome (alt text) has two causes. An
// unreadable or absent image is alt text for good and caching that is right; a heap refusal is a
// statement about one moment and must not be kept. These tests pin both sides.

struct ImageHeapGateFixture : testing::Test {
  fs::path work;
  std::string cacheDir;
  uint32_t savedHeap = 0;

  void SetUp() override {
    work = fs::temp_directory_path() /
           (std::string("epub_imgheap_") + testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(work);
    fs::create_directories(work);
    cacheDir = (work / "cache").string();
    fs::create_directories(cacheDir);
    savedHeap = ESP.getFreeHeap();
  }
  void TearDown() override {
    ESP.setFreeHeap(savedHeap);
    fs::remove_all(work);
  }

  // A one-chapter book whose <img> has no width/height and points at an entry that is NOT in the
  // archive. That is the only shape which reaches the heap gate on the host: an image the
  // manifest can resolve never gets there, and the manifest resolves anything whose header sits
  // in the first 4 KB.
  std::string makeBookWithUnresolvableImage() {
    test_zip::StoredZipWriter zip;
    zip.add("mimetype", "application/epub+zip");
    zip.add("META-INF/container.xml",
            "<?xml version=\"1.0\"?>\n<container version=\"1.0\" "
            "xmlns=\"urn:oasis:names:tc:opendocument:xmlns:container\">\n<rootfiles><rootfile "
            "full-path=\"content.opf\" media-type=\"application/oebps-package+xml\"/></rootfiles>\n</container>\n");
    zip.add("content.opf",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<package xmlns=\"http://www.idpf.org/2007/opf\" "
            "version=\"3.0\" unique-identifier=\"id\">\n<metadata "
            "xmlns:dc=\"http://purl.org/dc/elements/1.1/\"><dc:identifier id=\"id\">img</dc:identifier>"
            "<dc:title>Img</dc:title><dc:language>en</dc:language></metadata>\n<manifest>\n<item id=\"c\" "
            "href=\"chapter.xhtml\" media-type=\"application/xhtml+xml\"/>\n</manifest>\n<spine><itemref "
            "idref=\"c\"/></spine>\n</package>\n");
    zip.add("chapter.xhtml",
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<html xmlns=\"http://www.w3.org/1999/xhtml\">"
            "<head><title>C</title></head><body>\n<p>Before the image.</p>\n"
            "<img src=\"absent.png\" alt=\"alt text stands in\"/>\n<p>After the image.</p>\n</body></html>\n");
    const std::string path = (work / "img.epub").string();
    zip.write(path);
    return path;
  }

  bool buildAndReportDegraded(const std::string& bookPath, const std::string& cache,
                              uint32_t heapAfterFontCacheRelease = 0) {
    auto epub = std::make_shared<Epub>(bookPath, cache);
    EXPECT_TRUE(epub->load(true));
    Section::BuildParams params;
    params.viewportWidth = 480;
    params.viewportHeight = 800;
    params.lineCompression = 1.0f;
    GfxRenderer renderer;
    renderer.heapAfterFontCacheRelease = heapAfterFontCacheRelease;
    Section section(epub, 0, renderer);
    EXPECT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
    return section.isImageHeaderDegraded();
  }
};

TEST_F(ImageHeapGateFixture, HeapRefusalIsLatchedSoTheCacheCanBeDiscarded) {
  const std::string book = makeBookWithUnresolvableImage();

  // Between the text-layout hard abort (9 KB, below which the parse gives up entirely) and
  // EHP_IMAGE_HEADER_MIN_FREE_HEAP (16 KB): the parse runs to completion, and the header read is
  // refused before it is attempted. That ordering is the point of the ladder — an image is given
  // up long before the chapter is.
  ESP.setFreeHeap(12 * 1024);
  EXPECT_TRUE(buildAndReportDegraded(book, (work / "lowheap").string()))
      << "a heap refusal must be latched, or the alt-text page is cached forever";
}

// The latch has to survive the build. A starved rebuild cached a chapter with all of its images
// laid out as alt text, and nothing on the next open knew: the chapter stayed image-less even
// after a reboot had freed the heap that would have sized them (X3 2026-09-25). The flag now
// lives in the section header, where the reader's cache probe can see it.
TEST_F(ImageHeapGateFixture, HeapRefusalIsReadBackFromTheCachedSection) {
  const std::string book = makeBookWithUnresolvableImage();
  const std::string cache = (work / "persisted").string();
  ESP.setFreeHeap(12 * 1024);
  ASSERT_TRUE(buildAndReportDegraded(book, cache));
  ESP.setFreeHeap(200 * 1024);

  auto epub = std::make_shared<Epub>(book, cache);
  ASSERT_TRUE(epub->load(true));
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.loadSectionFile(params)) << "the degraded build is still a usable cache";
  EXPECT_TRUE(section.isImageHeaderDegraded()) << "a cold open must see that images were left out";
  EXPECT_FALSE(section.isTruncatedCache()) << "the flag shares a byte with parseComplete";
}

TEST_F(ImageHeapGateFixture, CleanBuildIsNotReadBackAsDegraded) {
  const std::string book = makeBookWithUnresolvableImage();
  const std::string cache = (work / "clean").string();
  ESP.setFreeHeap(200 * 1024);
  ASSERT_FALSE(buildAndReportDegraded(book, cache));

  auto epub = std::make_shared<Epub>(book, cache);
  ASSERT_TRUE(epub->load(true));
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  GfxRenderer renderer;
  Section section(epub, 0, renderer);
  ASSERT_TRUE(section.loadSectionFile(params));
  EXPECT_FALSE(section.isImageHeaderDegraded());
}

TEST_F(ImageHeapGateFixture, ReleasingFontCachesRecoversARefusedHeaderRead) {
  const std::string book = makeBookWithUnresolvableImage();

  // Same refusal as the latching test above, except this time there are font caches to
  // reclaim. The retry must lift free heap back over the gate and let the read proceed, so
  // the image is no longer given up for want of memory. It then fails on its own merits (the
  // entry does not exist), which is not a heap refusal and so must not be latched — the
  // inverse outcome of HeapRefusalIsLatchedSoTheCacheCanBeDiscarded, from the same start.
  ESP.setFreeHeap(12 * 1024);
  EXPECT_FALSE(buildAndReportDegraded(book, (work / "recovered").string(), /*heapAfterRelease=*/200 * 1024))
      << "releasing the font caches must let the header read proceed instead of degrading the build";
}

TEST_F(ImageHeapGateFixture, AnImageThatSimplyCannotBeReadIsNotLatched) {
  const std::string book = makeBookWithUnresolvableImage();

  // Ample heap: the read is attempted and fails on its own merits (the entry does not exist).
  // Same alt text on screen, but nothing transient about it — caching that is correct, so the
  // build must NOT be flagged for discard or the spine would rebuild forever.
  ESP.setFreeHeap(200 * 1024);
  EXPECT_FALSE(buildAndReportDegraded(book, (work / "fullheap").string()))
      << "an unreadable image is alt text for good; flagging it would cause endless rebuilds";
}

TEST_F(ImageHeapGateFixture, AResolvableImageIsNeverFlagged) {
  ESP.setFreeHeap(200 * 1024);
  auto epub = std::make_shared<Epub>(std::string(CORPUS_DIR) + "/test_png_images.epub", cacheDir);
  ASSERT_TRUE(epub->load(true));
  Section::BuildParams params;
  params.viewportWidth = 480;
  params.viewportHeight = 800;
  params.lineCompression = 1.0f;
  GfxRenderer renderer;
  Section section(epub, 5, renderer);  // chapter with an <img> carrying no dimensions
  ASSERT_TRUE(section.createSectionFile(params, {}, /*skipEviction=*/true));
  EXPECT_FALSE(section.isImageHeaderDegraded());
  EXPECT_GT(section.pageCount, 0);
}
