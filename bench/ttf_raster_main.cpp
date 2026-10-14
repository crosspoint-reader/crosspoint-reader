// Device bench for the TTF rasterizer (Stage 2b): every glyph of Bookerly Regular at 14 pt (the body size) and
// at 40 pt (a large heading). The font is embedded in the image (board_build.embed_files), so it is read
// through the flash cache, as the glyph store's mapped styles will be. Prints timings, the scratch block, the
// heap and the loop task's stack, and a hash of the output that must equal the host's
// (bench/ttf_raster_golden.h). Ends with "=== done ===".
//
// Build & flash: pio run -e bench_ttf_raster -t upload
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <ttf/TtfRasterizer.h>

#include "ttf_raster_golden.h"

extern const uint8_t kFontStart[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_start");
extern const uint8_t kFontEnd[] asm("_binary_lib_EpdFont_builtinFonts_source_Bookerly_Bookerly_Regular_ttf_end");

using namespace ttf;

namespace {

struct Run {
  uint32_t ok = 0;
  uint32_t failed = 0;
  int64_t totalUs = 0;
  int64_t maxUs = 0;
  uint64_t pixels = 0;
  uint32_t hash = ttf_golden::kSeed;
};

Run renderAll(Rasterizer& rasterizer, FontBytes& bytes, const Face& face, const uint32_t ppem26_6) {
  Run run;
  for (uint32_t g = 0; g < face.numGlyphs; ++g) {
    EpdGlyph metrics{};
    const int64_t start = esp_timer_get_time();
    const Status status = rasterizer.render(bytes, face, static_cast<uint16_t>(g), ppem26_6, &metrics);
    const int64_t us = esp_timer_get_time() - start;
    if (status != Status::Ok) {
      ++run.failed;
      continue;
    }
    ++run.ok;
    run.totalUs += us;
    run.maxUs = us > run.maxUs ? us : run.maxUs;
    run.pixels += static_cast<uint64_t>(metrics.width) * metrics.height;
    run.hash = ttf_golden::hashGlyph(run.hash, static_cast<uint16_t>(g), metrics, rasterizer.output());
  }
  return run;
}

void report(const char* name, const Run& run) {
  Serial.printf("BENCH %-20s glyphs=%u failed=%u avg=%lldus max=%lldus px/glyph=%u total=%lldms hash=%08x\n", name,
                static_cast<unsigned>(run.ok), static_cast<unsigned>(run.failed), run.ok ? run.totalUs / run.ok : 0LL,
                run.maxUs, run.ok ? static_cast<unsigned>(run.pixels / run.ok) : 0u, run.totalUs / 1000,
                static_cast<unsigned>(run.hash));
}

unsigned freeHeap() { return static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)); }
unsigned largestBlock() { return static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)); }

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== TTF rasterizer device bench (Stage 2b) ===");

  FontBytes bytes(kFontStart, static_cast<uint32_t>(kFontEnd - kFontStart));
  Face face;
  const FaceStatus parsed = parseFace(bytes, &face);
  Serial.printf("font %u B, parse %s, %u glyphs, %u units per em, maxp %u points %u contours\n",
                static_cast<unsigned>(bytes.size()), parsed == FaceStatus::Ok ? "ok" : "FAILED",
                static_cast<unsigned>(face.numGlyphs), static_cast<unsigned>(face.unitsPerEm),
                static_cast<unsigned>(face.maxPoints), static_cast<unsigned>(face.maxContours));
  if (parsed != FaceStatus::Ok) {
    Serial.println("=== done ===");
    return;
  }

  const uint32_t body = ppemFromPoints(14);
  const uint32_t heading = ppemFromPoints(40);
  Rasterizer rasterizer;
  const unsigned heapBefore = freeHeap();
  if (!rasterizer.allocate(planScratch(face, heading, kPoolBytesFlash))) {
    Serial.println("FATAL: scratch block");
    Serial.println("=== done ===");
    return;
  }
  Serial.printf("scratch %u B (heap %u -> %u B, largest block %u B); output region %u B\n",
                static_cast<unsigned>(rasterizer.scratchBytes()), heapBefore, freeHeap(), largestBlock(),
                static_cast<unsigned>(rasterizer.plan().outputBytes));

  const unsigned stackBefore = static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr));
  const Run text = renderAll(rasterizer, bytes, face, body);
  report("render_14pt_pool4k", text);
  Serial.printf("output hash %08x: %s the host's %08x\n", static_cast<unsigned>(text.hash),
                text.hash == ttf_golden::kBookerlyRegular14pt ? "MATCHES" : "DIFFERS FROM",
                static_cast<unsigned>(ttf_golden::kBookerlyRegular14pt));
  report("render_40pt_pool4k", renderAll(rasterizer, bytes, face, heading));
  Serial.printf("loop task stack: lowest free %u B before rendering, %u B after\n", stackBefore,
                static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));

  // The pool is a speed dial (spec 2): the same glyphs with 2 KB and with the SD mode's 8 KB.
  rasterizer.allocate(planScratch(face, heading, 2048));
  report("render_14pt_pool2k", renderAll(rasterizer, bytes, face, body));
  rasterizer.allocate(planScratch(face, heading, kPoolBytesSd));
  report("render_14pt_pool8k", renderAll(rasterizer, bytes, face, body));
  report("render_40pt_pool8k", renderAll(rasterizer, bytes, face, heading));
  rasterizer.release();
  Serial.printf("after release: heap %u B\n", freeHeap());
  Serial.println("=== done ===");
}

void loop() { delay(30000); }
