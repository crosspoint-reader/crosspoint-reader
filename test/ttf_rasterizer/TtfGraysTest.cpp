// The vendored FreeType rasterizer (lib/EpdFont/ttf/third_party/freetype): it renders in the caller's cell
// pool, never on the stack, and the pool's size changes nothing but the number of bands.
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <utility>
#include <vector>

#include "TtfGrays.h"

namespace {

// An outline of straight-edged contours in 26.6, every point on the curve.
struct Polygon {
  std::vector<FT_Vector> points;
  std::vector<unsigned char> tags;
  std::vector<unsigned short> contours;

  void add(const std::initializer_list<std::pair<long, long>> corners) {
    for (const auto& [x, y] : corners) {
      points.push_back({x, y});
      tags.push_back(FT_CURVE_TAG_ON);
    }
    contours.push_back(static_cast<unsigned short>(points.size() - 1));
  }
  FT_Outline outline() {
    FT_Outline o{};
    o.n_points = static_cast<unsigned short>(points.size());
    o.n_contours = static_cast<unsigned short>(contours.size());
    o.points = points.data();
    o.tags = tags.data();
    o.contours = contours.data();
    return o;
  }
};

// Coverage per pixel, top row first, from the spans ftgrays emits (it counts rows from the bottom).
struct Image {
  int width;
  int height;
  std::vector<int> coverage;
};

void accumulate(const int y, const int count, const FT_Span* spans, void* user) {
  auto* image = static_cast<Image*>(user);
  for (int i = 0; i < count; ++i) {
    for (int x = spans[i].x; x < spans[i].x + spans[i].len; ++x) {
      image->coverage[(image->height - 1 - y) * image->width + x] += spans[i].coverage;
    }
  }
}

int render(Polygon& polygon, Image* image, unsigned char* pool, const unsigned long poolBytes) {
  image->coverage.assign(static_cast<size_t>(image->width * image->height), 0);
  FT_Outline outline = polygon.outline();
  FT_Raster_Params params{};
  params.source = &outline;
  params.flags = FT_RASTER_FLAG_AA | FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_CLIP;
  params.gray_spans = accumulate;
  params.user = image;
  params.clip_box.xMax = image->width;
  params.clip_box.yMax = image->height;
  FT_Raster raster = nullptr;
  ttf_grays_raster.raster_new(nullptr, &raster);
  ttf_grays_raster.raster_reset(raster, pool, poolBytes);
  return ttf_grays_raster.raster_render(raster, &params);
}

// A diamond 60 px across with a diamond hole: enough edge cells to need several bands in a 2 KB pool.
Polygon diamond() {
  Polygon polygon;
  polygon.add({{32 * 64, 2 * 64}, {62 * 64, 32 * 64}, {32 * 64, 62 * 64}, {2 * 64, 32 * 64}});
  polygon.add({{32 * 64, 22 * 64}, {22 * 64, 32 * 64}, {32 * 64, 42 * 64}, {42 * 64, 32 * 64}});  // reversed
  return polygon;
}

}  // namespace

TEST(TtfGrays, RendersInTheCallersPool) {
  Polygon square;
  square.add({{0, 0}, {256, 0}, {256, 256}, {0, 256}});  // 4 x 4 px
  alignas(8) unsigned char pool[4096];
  std::memset(pool, 0xA5, sizeof pool);
  Image image{4, 4, {}};
  ASSERT_EQ(0, render(square, &image, pool, sizeof pool));
  for (const int c : image.coverage) EXPECT_EQ(255, c);
  bool used = false;
  for (const unsigned char byte : pool) used |= byte != 0xA5;
  EXPECT_TRUE(used);
}

TEST(TtfGrays, WithoutAPoolNothingIsRendered) {
  Polygon square;
  square.add({{0, 0}, {256, 0}, {256, 256}, {0, 256}});
  Image image{4, 4, {}};
  EXPECT_NE(0, render(square, &image, nullptr, 0));
  alignas(8) unsigned char small[1024];
  EXPECT_NE(0, render(square, &image, small, sizeof small));  // under the 2 KB minimum
  for (const int c : image.coverage) EXPECT_EQ(0, c);
}

TEST(TtfGrays, ThePoolSizeChangesOnlyTheBands) {
  Polygon shape = diamond();
  alignas(8) static unsigned char pool[16384];
  Image small{64, 64, {}};
  Image large{64, 64, {}};
  ASSERT_EQ(0, render(shape, &small, pool, 2048));
  ASSERT_EQ(0, render(shape, &large, pool, sizeof pool));
  EXPECT_EQ(large.coverage, small.coverage);
  int inked = 0;
  for (const int c : large.coverage) inked += c > 0;
  EXPECT_GT(inked, 1500);  // 2 * 30^2 less the hole's 2 * 10^2, plus the edges
}
