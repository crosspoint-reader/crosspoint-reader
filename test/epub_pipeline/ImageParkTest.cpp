// Parked image decodes (RenderConfig::checkpointPath).
//
// The reader's image lane hands its region back on every page turn, so a decode it cannot finish
// is stopped by CooperativeAbort. It used to be thrown away and started over -- in runs 18-21 (X3)
// the lane discarded 73 s of preempted decodes against 54 s it completed. Now the decode parks: the
// rows already done stay in the caches' .part files, a checkpoint holds the decoder's and the
// pipeline's state, and the next decode of the same image resumes.
//
// The guarantee tested here: however often a decode is stopped, and wherever, the caches it
// finally publishes are byte for byte those of an uninterrupted decode -- both variants, for the
// full progressive decoder and for TJpgDec.
#include <esp_heap_caps.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "CooperativeAbort.h"
#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/JpegToFramebufferConverter.h"
#include "Epub/converters/PixelCache.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "ProgressiveJpeg.h"

namespace fs = std::filesystem;

namespace {

// Fires on the Nth poll of the abort hook (1-based); 0 never fires.
int gPollsLeft = 0;
int gPolls = 0;
bool countedAbort() {
  ++gPolls;
  if (gPollsLeft <= 0) return false;
  return --gPollsLeft == 0;
}

struct ImageParkFixture : testing::TestWithParam<const char*> {
  fs::path work;
  GfxRenderer renderer;

  void SetUp() override {
    const std::string name = testing::UnitTest::GetInstance()->current_test_info()->name();
    work = fs::temp_directory_path() / ("img_park_" + std::to_string(std::hash<std::string>{}(name)));
    fs::remove_all(work);
    fs::create_directories(work);
    CooperativeAbort::setLongTaskAbortPredicate(countedAbort);
    gPollsLeft = 0;
    gPolls = 0;
    CooperativeAbort::clearAborted();
  }
  void TearDown() override {
    CooperativeAbort::setLongTaskAbortPredicate(nullptr);
    CooperativeAbort::clearAborted();
    fs::remove_all(work);
  }

  std::string image() const { return std::string(JPEG_FIXTURE_DIR "/") + GetParam(); }
  std::string at(const std::string& name) const { return (work / name).string(); }

  // The reader's configuration: the 1-bit variant drawn, the 4-level one as its companion, a
  // downscale that area-averages (so the carry across rows of blocks is in play).
  RenderConfig config(const std::string& tag, const int width = 150, const int height = 104,
                      const bool checkpoint = true) const {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = width;
    c.maxHeight = height;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = at(tag + "_bw.pxc");
    c.companionCachePath = at(tag + "_grey.pxc");
    if (checkpoint) c.checkpointPath = at(tag + ".ckpt");
    return c;
  }

  // One decode call; true when it stopped for input (the lane's "preempted").
  bool decodeOnce(const RenderConfig& c, const int abortOnPoll) {
    gPollsLeft = abortOnPoll;
    CooperativeAbort::clearAborted();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(image(), renderer, c));
    gPollsLeft = 0;
    return CooperativeAbort::consumeAborted();
  }

  static std::vector<uint8_t> read(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  }
};

TEST_P(ImageParkFixture, EveryParkPointResumesToTheUninterruptedCaches) {
  const RenderConfig ref = config("ref", 150, 104, /*checkpoint=*/false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const auto refBw = read(ref.cachePath);
  const auto refGrey = read(ref.companionCachePath);
  ASSERT_GT(refBw.size(), PixelCache::PXC_HEADER_BYTES);
  ASSERT_EQ(refBw.size(), refGrey.size());
  ASSERT_NE(refBw, refGrey);

  // How many times an uninterrupted checkpointable decode polls the hook: every one is a park point.
  const RenderConfig probe = config("probe");
  gPolls = 0;
  ASSERT_FALSE(decodeOnce(probe, 0));
  const int polls = gPolls;
  ASSERT_GT(polls, 3) << "the fixture must be stoppable at several points";
  EXPECT_EQ(read(probe.cachePath), refBw) << "checkpointing must not change an uninterrupted decode";

  for (int stop = 1; stop <= polls; ++stop) {
    SCOPED_TRACE("stopped at poll " + std::to_string(stop));
    const RenderConfig c = config("p" + std::to_string(stop));
    ASSERT_TRUE(decodeOnce(c, stop));
    EXPECT_FALSE(fs::exists(c.cachePath)) << "a parked decode publishes nothing";
    EXPECT_TRUE(fs::exists(c.checkpointPath));
    EXPECT_TRUE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    ASSERT_FALSE(decodeOnce(c, 0)) << "the resumed decode runs to the end";
    EXPECT_FALSE(fs::exists(c.checkpointPath)) << "a finished decode leaves no checkpoint";
    EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
    EXPECT_EQ(read(c.cachePath), refBw);
    EXPECT_EQ(read(c.companionCachePath), refGrey);
  }
}

// A decode stopped again and again -- a reader flipping pages -- still ends with the same caches.
TEST_P(ImageParkFixture, RepeatedParksStillEndInTheSameCaches) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));
  const RenderConfig c = config("many");
  int parks = 0;
  while (decodeOnce(c, 2)) {  // every call makes one poll's worth of progress, then parks
    ++parks;
    ASSERT_LT(parks, 1000);
  }
  EXPECT_GT(parks, 1);
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

// A checkpoint only resumes the decode it was taken of: another box starts over, cleanly.
TEST_P(ImageParkFixture, AChangedConfigurationStartsOver) {
  const RenderConfig small = config("ref_small", 120, 83, false);
  ASSERT_FALSE(decodeOnce(small, 0));

  RenderConfig c = config("changed");
  ASSERT_TRUE(decodeOnce(c, 3));
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  c.maxWidth = 120;
  c.maxHeight = 83;
  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_EQ(read(c.cachePath), read(small.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(small.companionCachePath));
}

// A partial cache that no longer matches its checkpoint (a crash between the two writes, a
// card error) fails that one decode and clears the way: the next one starts over.
TEST_P(ImageParkFixture, ADamagedPartialFileStartsOverNextTime) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));

  const RenderConfig c = config("damaged");
  ASSERT_TRUE(decodeOnce(c, 3));
  const std::string part = PixelCache::partPathFor(c.cachePath);
  fs::resize_file(part, fs::file_size(part) + 1);

  JpegToFramebufferConverter converter;
  EXPECT_FALSE(converter.decodeToFramebuffer(image(), renderer, c)) << "this decode fails";
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_FALSE(fs::exists(part));
  EXPECT_FALSE(fs::exists(c.cachePath));

  ASSERT_FALSE(decodeOnce(c, 0));
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
}

// A pass that cannot cache (the heap gate refused one) must not throw a parked decode away: it
// draws without touching the parked partial files, and the next pass with room resumes. It used to
// discard the checkpoint, so a page render on a tight heap undid the image lane's work.
TEST_P(ImageParkFixture, APassWithoutRoomToCacheLeavesTheParkedDecode) {
  const RenderConfig ref = config("ref", 150, 104, false);
  ASSERT_FALSE(decodeOnce(ref, 0));

  const RenderConfig c = config("tight");
  ASSERT_TRUE(decodeOnce(c, 3));
  const std::string part = PixelCache::partPathFor(c.cachePath);
  ASSERT_TRUE(fs::exists(c.checkpointPath));
  const auto partBefore = read(part);
  const auto checkpointBefore = read(c.checkpointPath);

  // Room to decode (a progressive decode needs 16 KB), none to cache (band + 20 KB).
  const uint32_t savedHeap = ESP.getFreeHeap();
  ESP.setFreeHeap(18 * 1024);
  {
    JpegToFramebufferConverter converter;
    converter.decodeToFramebuffer(image(), renderer, c);  // baseline refuses outright below 28 KB
  }
  ESP.setFreeHeap(savedHeap);

  EXPECT_FALSE(fs::exists(c.cachePath)) << "nothing was cached";
  EXPECT_EQ(read(c.checkpointPath), checkpointBefore) << "the parked decode is left as it was";
  EXPECT_EQ(read(part), partBefore);

  ASSERT_FALSE(decodeOnce(c, 0)) << "the next pass with room resumes and finishes";
  EXPECT_FALSE(fs::exists(c.checkpointPath));
  EXPECT_EQ(read(c.cachePath), read(ref.cachePath));
  EXPECT_EQ(read(c.companionCachePath), read(ref.companionCachePath));
}

// Without a checkpoint path a stopped decode is thrown away, as before, and leaves nothing.
TEST_P(ImageParkFixture, WithoutACheckpointPathAStopIsDiscarded) {
  const RenderConfig c = config("nockpt", 150, 104, false);
  const bool stopped = decodeOnce(c, 2);
  if (!stopped) GTEST_SKIP() << "this decoder only polls when it can park";
  EXPECT_FALSE(fs::exists(c.cachePath));
  EXPECT_FALSE(fs::exists(PixelCache::partPathFor(c.cachePath)));
}

// prog_full_420_base_rst.jpg: baseline with a restart marker every 3 MCUs, so a parked TJpgDec
// decode resumes with the restart counters mid-interval and, every third row, on a boundary.
INSTANTIATE_TEST_SUITE_P(ProgressiveAndBaseline, ImageParkFixture,
                         testing::Values("prog_full_420.jpg", "prog_full_420_base.jpg", "prog_full_gray.jpg",
                                         "prog_full_420_base_rst.jpg"));

// Guards the restart fixture itself: restart markers only change the entropy coding, so the same
// picture with and without them must decode to the same caches -- else the park tests above would
// compare a resumed decode against a wrong reference and prove nothing.
TEST(ImageParkRestartFixture, RestartMarkersDecodeLikeTheirTwin) {
  const fs::path work = fs::temp_directory_path() / "img_park_rst_twin";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const auto decodeTo = [&](const char* jpg, const std::string& tag) {
    RenderConfig c;
    c.x = 12;
    c.y = 30;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = (work / (tag + "_bw.pxc")).string();
    c.companionCachePath = (work / (tag + "_grey.pxc")).string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(std::string(JPEG_FIXTURE_DIR "/") + jpg, renderer, c));
    std::ifstream bw(c.cachePath, std::ios::binary);
    std::ifstream grey(c.companionCachePath, std::ios::binary);
    std::vector<uint8_t> out{std::istreambuf_iterator<char>(bw), std::istreambuf_iterator<char>()};
    out.insert(out.end(), std::istreambuf_iterator<char>(grey), std::istreambuf_iterator<char>());
    return out;
  };
  const auto plain = decodeTo("prog_full_420_base.jpg", "plain");
  ASSERT_GT(plain.size(), 2 * PixelCache::PXC_HEADER_BYTES);
  EXPECT_EQ(decodeTo("prog_full_420_base_rst.jpg", "rst"), plain);
  fs::remove_all(work);
}

// A progressive JPEG whose full decode finds no room for its workspace is drawn from its DC preview: the cache is
// stamped coarse, for a pass with room to decode again, and the decode counts as short of memory and says what a
// finer one needs, so the reader's memory ladder can tell whether making room is worth it. A full decode is neither.
TEST(CoarseProgressiveJpeg, APreviewForLackOfRoomIsStampedCoarseAndShortOfMemory) {
  const fs::path work = fs::temp_directory_path() / "img_coarse_preview";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_full_420.jpg";
  ProgressiveJpeg::ImageInfo info;
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("TEST", jpg, file));
    ASSERT_EQ(ProgressiveJpeg::Result::Ok, ProgressiveJpeg::probe(file, info));
    file.close();
  }
  const auto decodeMagic = [&](const std::string& tag) {
    RenderConfig c;
    c.maxWidth = 150;
    c.maxHeight = 104;
    c.useExactDimensions = true;
    c.monochromeOutput = true;
    c.cachePath = (work / (tag + "_bw.pxc")).string();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, c));
    std::ifstream in(c.cachePath, std::ios::binary);
    uint16_t magic = 0;
    in.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    return magic;
  };
  const uint32_t savedHeap = ESP.getFreeHeap();
  // A byte short of the smallest workspace and the 20 KB it must leave (JPEG_CACHE_POST_GATE_BYTES +
  // JPEG_CACHE_HEAP_FLOOR): room for the preview and its cache band still.
  const auto coarsest = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3));
  ESP.setFreeHeap(coarsest + 20 * 1024 - 1);
  image_scratch::takeCoarseNeed();
  uint32_t shortBefore = image_scratch::heapShortCount();
  EXPECT_EQ(PixelCache::PXC_MAGIC_COARSE, decodeMagic("preview"));
  EXPECT_NE(shortBefore, image_scratch::heapShortCount());
  const image_scratch::CoarseNeed need = image_scratch::takeCoarseNeed();
  EXPECT_EQ(coarsest, need.bytes) << "none had room: the coarsest workspace is the next finer decode";
  EXPECT_EQ(20u * 1024, need.heapFloor);
  EXPECT_FALSE(image_scratch::coarseNeedFits(need)) << "no more room than the decode had";
  EXPECT_TRUE(image_scratch::coarseNeedFits(need, 1)) << "a byte freed first is room enough";
  ESP.setFreeHeap(savedHeap);

  shortBefore = image_scratch::heapShortCount();
  EXPECT_EQ(PixelCache::PXC_MAGIC, decodeMagic("full"));
  EXPECT_EQ(shortBefore, image_scratch::heapShortCount());
  EXPECT_EQ(0u, image_scratch::takeCoarseNeed().bytes);
  fs::remove_all(work);
}

// The check the memory ladder makes before it frees memory for a coarse picture, and again before it decodes, is the
// decoder's own: where it says the finer workspace fits, the decode gets it; a byte short, the decode does not.
TEST(CoarseProgressiveJpeg, WhereTheFinerWorkspaceFitsTheDecodeIsFiner) {
  const fs::path work = fs::temp_directory_path() / "img_coarse_finer";
  fs::remove_all(work);
  fs::create_directories(work);
  GfxRenderer renderer;
  const std::string jpg = JPEG_FIXTURE_DIR "/prog_full_420.jpg";
  ProgressiveJpeg::ImageInfo info;
  {
    FsFile file;
    ASSERT_TRUE(Storage.openFileForRead("TEST", jpg, file));
    ASSERT_EQ(ProgressiveJpeg::Result::Ok, ProgressiveJpeg::probe(file, info));
    file.close();
  }
  RenderConfig c;
  c.maxWidth = 150;
  c.maxHeight = 104;
  c.useExactDimensions = true;
  c.monochromeOutput = true;
  c.cachePath = (work / "bw.pxc").string();
  // The same image decoded again over its cache, as the light step does: what does the decode need next?
  const auto decodeNeed = [&](const uint32_t freeHeap) {
    ESP.setFreeHeap(freeHeap);
    image_scratch::takeCoarseNeed();
    JpegToFramebufferConverter converter;
    EXPECT_TRUE(converter.decodeToFramebuffer(jpg, renderer, c));
    return image_scratch::takeCoarseNeed();
  };
  const uint32_t savedHeap = ESP.getFreeHeap();
  const auto coarsest = static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 3));
  const image_scratch::CoarseNeed need = decodeNeed(coarsest + 20 * 1024 - 1);  // the DC preview
  ASSERT_EQ(coarsest, need.bytes);

  ESP.setFreeHeap(need.bytes + need.heapFloor - 1);
  EXPECT_FALSE(image_scratch::coarseNeedFits(need));
  EXPECT_EQ(coarsest, decodeNeed(need.bytes + need.heapFloor - 1).bytes) << "a byte short: the preview again";

  ESP.setFreeHeap(need.bytes + need.heapFloor);
  EXPECT_TRUE(image_scratch::coarseNeedFits(need));
  {
    const HostLargestFreeBlock fragmented(need.bytes);
    EXPECT_FALSE(image_scratch::coarseNeedFits(need)) << "free in all, but in no one block";
  }
  // The finer decode: the full decoder at its coarsest scale, not the DC preview, so the next finer is one up.
  EXPECT_EQ(static_cast<uint32_t>(ProgressiveJpeg::workspaceBytes(info, 2)),
            decodeNeed(need.bytes + need.heapFloor).bytes);
  ESP.setFreeHeap(savedHeap);
  fs::remove_all(work);
}

}  // namespace
