#include <gtest/gtest.h>
#include <zlib.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "InflateReader.h"

// The vendored uzlib ships without its checksum sources; the firmware never
// calls uzlib_uncompress_chksum, so the linker drops the references there.
extern "C" uint32_t uzlib_adler32(const void*, unsigned int, uint32_t) { std::abort(); }
extern "C" uint32_t uzlib_crc32(const void*, unsigned int, uint32_t) { std::abort(); }

namespace {

// Raw deflate (no zlib/gzip wrapper) with the full 32KB window, as dictzip uses.
std::vector<uint8_t> deflateRaw(const std::vector<uint8_t>& in) {
  z_stream zs{};
  EXPECT_EQ(deflateInit2(&zs, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY), Z_OK);
  std::vector<uint8_t> out(deflateBound(&zs, in.size()));
  zs.next_in = const_cast<uint8_t*>(in.data());
  zs.avail_in = static_cast<uInt>(in.size());
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  EXPECT_EQ(deflate(&zs, Z_FINISH), Z_STREAM_END);
  out.resize(zs.total_out);
  deflateEnd(&zs);
  return out;
}

// Incompressible noise interleaved with copies from 1..32767 bytes back, so the
// stream carries back-references at every distance class, including ones that
// straddle each ring-segment boundary and reach the far end of the window.
std::vector<uint8_t> makeLongDistanceData(size_t size) {
  std::vector<uint8_t> data;
  data.reserve(size);
  uint32_t lcg = 12345;
  const auto next = [&lcg] {
    lcg = lcg * 1103515245u + 12345u;
    return lcg >> 16;
  };
  while (data.size() < size) {
    for (int i = 0; i < 64 && data.size() < size; i++) data.push_back(static_cast<uint8_t>(next()));
    if (data.size() < 40000) continue;
    const size_t distance = 1 + next() % 32767;
    const size_t len = 3 + next() % 255;
    for (size_t i = 0; i < len && data.size() < size; i++) data.push_back(data[data.size() - distance]);
  }
  return data;
}

struct StreamCtx {
  InflateReader reader;  // must be first
  const uint8_t* src = nullptr;
  size_t remaining = 0;
};

// Feeds input 2KB at a time, the way DictZip pulls compressed bytes from SD.
int feedCb(uzlib_uncomp* u) {
  auto* ctx = reinterpret_cast<StreamCtx*>(u);
  if (ctx->remaining == 0) return -1;
  const size_t n = std::min<size_t>(ctx->remaining, 2048);
  const uint8_t* chunk = ctx->src;
  ctx->src += n;
  ctx->remaining -= n;
  u->source = chunk + 1;
  u->source_limit = chunk + n;
  return chunk[0];
}

TEST(InflateReader, SegmentedRingDecodesFullWindowBackReferences) {
  const std::vector<uint8_t> plain = makeLongDistanceData(160 * 1024);
  const std::vector<uint8_t> packed = deflateRaw(plain);

  // Separate allocations: the decoder must never assume the segments are adjacent.
  std::unique_ptr<uint8_t[]> owned[InflateReader::RING_SEGMENTS];
  uint8_t* segments[InflateReader::RING_SEGMENTS];
  for (size_t i = 0; i < InflateReader::RING_SEGMENTS; i++) {
    owned[i] = std::make_unique<uint8_t[]>(InflateReader::RING_SEGMENT_BYTES);
    segments[i] = owned[i].get();
  }

  auto ctx = std::make_unique<StreamCtx>();
  ctx->src = packed.data();
  ctx->remaining = packed.size();
  ASSERT_TRUE(ctx->reader.initWithSegments(segments));
  ctx->reader.setReadCallback(&feedCb);

  std::vector<uint8_t> out(plain.size());
  for (size_t pos = 0; pos < out.size(); pos += 512) {
    const size_t batch = std::min<size_t>(512, out.size() - pos);
    ASSERT_TRUE(ctx->reader.read(out.data() + pos, batch)) << "at " << pos;
  }
  EXPECT_EQ(out, plain);
}

TEST(InflateReader, InitWithSegmentsRejectsMissingSegment) {
  uint8_t a[InflateReader::RING_SEGMENT_BYTES];
  uint8_t* segments[InflateReader::RING_SEGMENTS] = {a, a, nullptr, a};
  InflateReader reader;
  EXPECT_FALSE(reader.initWithSegments(segments));
}

TEST(InflateReader, OneShotModeStillDecodes) {
  const std::vector<uint8_t> plain = makeLongDistanceData(48 * 1024);
  const std::vector<uint8_t> packed = deflateRaw(plain);

  InflateReader reader;
  ASSERT_TRUE(reader.init(false));
  reader.setSource(packed.data(), packed.size());
  std::vector<uint8_t> out(plain.size());
  ASSERT_TRUE(reader.read(out.data(), out.size()));
  EXPECT_EQ(out, plain);
}

TEST(InflateReader, ContiguousStreamingRingStillDecodes) {
  const std::vector<uint8_t> plain = makeLongDistanceData(96 * 1024);
  const std::vector<uint8_t> packed = deflateRaw(plain);

  auto ctx = std::make_unique<StreamCtx>();
  ctx->src = packed.data();
  ctx->remaining = packed.size();
  ASSERT_TRUE(ctx->reader.init(true));
  ctx->reader.setReadCallback(&feedCb);

  std::vector<uint8_t> out(plain.size());
  for (size_t pos = 0; pos < out.size(); pos += 512) {
    const size_t batch = std::min<size_t>(512, out.size() - pos);
    ASSERT_TRUE(ctx->reader.read(out.data() + pos, batch)) << "at " << pos;
  }
  EXPECT_EQ(out, plain);
}

}  // namespace
