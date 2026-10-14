// Device self-test for the TTF glyph store (Stage 2a): FlashGlyphStore through EspNorFlash on the real
// `spiffs` partition. Prints timings and checks, and ends with "=== done ===".
//
// It ERASES the partition's font area, which holds the .cpfont flash cache: the reader copies a .cpfont
// family back from the SD card the next time it needs one. The language slot at the end of the partition
// must come through untouched, which the test checks.
//
// Build & flash: pio run -e bench_glyph_store -t upload
#include <Arduino.h>
#include <EspNorFlash.h>
#include <esp_partition.h>
#include <esp_timer.h>
#include <ttf/FlashGlyphStore.h>
#include <ttf/GlyphStoreFormat.h>

using namespace glyphstore;
using Lookup = FlashGlyphStore::Lookup;
using Commit = FlashGlyphStore::Commit;

namespace {

constexpr uint8_t kIdentity[kIdentityBytes] = {'g', 'l', 'y', 'p', 'h', '-', 'b', 'e', 'n', 'c', 'h'};
constexpr uint32_t kReaderMapPages = 32;  // what the reader image leaves free (measured on the X3)
constexpr uint32_t kStyleBytes = 300000;
constexpr uint32_t kBatched = 3000;
constexpr uint32_t kSingles = 200;
constexpr uint32_t kGlyphs = kBatched + kSingles;
constexpr uint32_t kPageKeys = 60;    // distinct glyphs on a page of prose
constexpr uint32_t kPageRounds = 20;  // about 1,200 lookups, as on one page
constexpr uint32_t kPairs = 500;
alignas(4) uint8_t gBatch[4096];
alignas(4) uint8_t gChunk[4096];
uint32_t gKeys[kGlyphs];

uint32_t langSlotHash() {
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "spiffs");
  if (!part) return 0;
  uint32_t hash = fnv1a(nullptr, 0);
  for (uint32_t at = part->size - 65536; at < part->size; at += sizeof gChunk) {
    if (esp_partition_read(part, at, gChunk, sizeof gChunk) != ESP_OK) return 0;
    hash = fnv1a(gChunk, sizeof gChunk, hash);
  }
  return hash;
}

EpdGlyph shapeOf(const uint32_t i) {
  EpdGlyph glyph{};
  glyph.width = static_cast<uint8_t>(6 + i % 19);
  glyph.height = static_cast<uint8_t>(9 + i % 23);
  glyph.advanceX = static_cast<uint16_t>((glyph.width + 1) << 4);
  glyph.left = 1;
  glyph.top = glyph.height;
  glyph.dataLength = glyphDataBytes(glyph.width, glyph.height, true);
  return glyph;
}

uint8_t patternOf(const uint32_t key, const uint32_t j) { return static_cast<uint8_t>((key * 31u + j * 7u) ^ 0x5Au); }

bool stage(GlyphBatch& batch, const uint32_t i) {
  const EpdGlyph glyph = shapeOf(i);
  uint8_t* bitmap = batch.add(gKeys[i], glyph);
  if (!bitmap) return false;
  for (uint32_t j = 0; j < glyph.dataLength; ++j) bitmap[j] = patternOf(gKeys[i], j);
  return true;
}

uint32_t countBad(const FlashGlyphStore& store) {
  uint32_t bad = 0;
  for (uint32_t i = 0; i < kGlyphs; ++i) {
    const EpdGlyph want = shapeOf(i);
    const EpdGlyph* glyph = nullptr;
    if (store.find(gKeys[i], &glyph) != Lookup::Found || glyph->width != want.width || glyph->height != want.height ||
        glyph->dataLength != want.dataLength) {
      ++bad;
      continue;
    }
    const uint8_t* bitmap = store.logBase() + glyph->dataOffset;
    for (uint32_t j = 0; j < glyph->dataLength; ++j) {
      if (bitmap[j] != patternOf(gKeys[i], j)) {
        ++bad;
        break;
      }
    }
  }
  return bad;
}

unsigned msSince(const int64_t start) { return static_cast<unsigned>((esp_timer_get_time() - start) / 1000); }

void finish(const char* fatal) {
  if (fatal) Serial.printf("FATAL: %s\n", fatal);
  Serial.println("=== done ===");
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== Glyph store device self-test (TTF Stage 2a) ===");
  const uint32_t langBefore = langSlotHash();

  EspNorFlash flash;
  if (!flash.begin()) return finish("no spiffs partition");
  Serial.printf("font area %u B, free map pages %u (planning with %u, the reader's budget), free heap %u B\n",
                static_cast<unsigned>(flash.size()), static_cast<unsigned>(flash.freeMapPages()),
                static_cast<unsigned>(kReaderMapPages), static_cast<unsigned>(esp_get_free_heap_size()));

  PlanInput in{};
  in.mode = kModeFlash;
  in.styleCount = 2;
  in.styleBytes[0] = in.styleBytes[1] = kStyleBytes;
  in.usableBytes = flash.size();
  in.freeMapPages = flash.freeMapPages() < kReaderMapPages ? flash.freeMapPages() : kReaderMapPages;
  Layout layout{};
  if (!planLayout(in, layout)) return finish("planLayout refused the budget");
  const Geometry& g = layout.geometry;
  Serial.printf("layout: index %u slots at %u, log %u KB at %u\n", static_cast<unsigned>(g.indexSlots),
                static_cast<unsigned>(g.indexOffset), static_cast<unsigned>(g.logSize / 1024),
                static_cast<unsigned>(g.logOffset));

  FlashGlyphStore store(flash);
  int64_t t = esp_timer_get_time();
  bool ok = store.beginInstall(layout, kIdentity, nullptr);
  Serial.printf("BENCH install_erase     %u ms (only sectors that hold data)\n", msSince(t));
  t = esp_timer_get_time();
  for (uint8_t s = 0; ok && s < 2; ++s) {
    for (uint32_t at = 0; ok && at < kStyleBytes; at += sizeof gChunk) {
      const uint32_t len = kStyleBytes - at < sizeof gChunk ? kStyleBytes - at : static_cast<uint32_t>(sizeof gChunk);
      for (uint32_t j = 0; j < len; ++j) gChunk[j] = static_cast<uint8_t>(at + j + s);
      ok = store.writeStyleData(s, at, gChunk, len);
    }
  }
  ok = ok && store.finishInstall();
  Serial.printf("BENCH install_copy      %u ms for %u KB\n", msSince(t), static_cast<unsigned>(2 * kStyleBytes / 1024));
  t = esp_timer_get_time();
  if (!ok || store.open(kIdentity) != FlashGlyphStore::OpenResult::Ready) return finish("install or open failed");
  Serial.printf("BENCH open_empty        %u ms\n", msSince(t));

  uint8_t body = 0;
  uint8_t heading = 0;
  if (!store.sizeIndex(29 << 6, &body) || !store.sizeIndex(54 << 6, &heading)) return finish("sizeIndex failed");
  for (uint32_t i = 0; i < kGlyphs; ++i) {
    gKeys[i] = makeKey(0x21 + i, static_cast<uint8_t>(i % 2), i % 3 == 0 ? heading : body);
  }

  // Batched, as prefetch will commit a text run's glyphs.
  GlyphBatch batch(gBatch, sizeof gBatch);
  unsigned batches = 0;
  t = esp_timer_get_time();
  for (uint32_t i = 0; i < kBatched; ++i) {
    if (stage(batch, i)) continue;
    if (store.commit(batch) != Commit::Stored) return finish("batched commit failed");
    ++batches;
    if (!stage(batch, i)) return finish("a glyph did not fit an empty batch");
  }
  if (store.commit(batch) != Commit::Stored) return finish("last batched commit failed");
  ++batches;
  const int64_t batchedUs = esp_timer_get_time() - t;

  // One at a time, as the per-glyph miss path will.
  t = esp_timer_get_time();
  for (uint32_t i = kBatched; i < kGlyphs; ++i) {
    GlyphBatch one(gBatch, sizeof gBatch);
    if (!stage(one, i) || store.commit(one) != Commit::Stored) return finish("single commit failed");
  }
  const int64_t singleUs = esp_timer_get_time() - t;
  Serial.printf("BENCH commit_batched    per_glyph=%lldus  glyphs=%u  batches=%u\n", batchedUs / kBatched,
                static_cast<unsigned>(kBatched), batches);
  Serial.printf("BENCH commit_single     per_glyph=%lldus  glyphs=%u\n", singleUs / kSingles,
                static_cast<unsigned>(kSingles));
  Serial.printf("verify after writes:    %u bad of %u\n", static_cast<unsigned>(countBad(store)),
                static_cast<unsigned>(kGlyphs));

  // Lookups through the mapping: a page's few glyphs again and again (warm), then 3,000 glyphs round robin,
  // which misses the 16 KB flash cache on nearly every lookup (cold).
  unsigned found = 0;
  t = esp_timer_get_time();
  for (uint32_t r = 0; r < kPageRounds; ++r) {
    for (uint32_t i = 0; i < kPageKeys; ++i) found += store.find(gKeys[i * 7], nullptr) == Lookup::Found;
  }
  const int64_t warmUs = esp_timer_get_time() - t;
  Serial.printf("BENCH find_warm         per_lookup=%lldns  found=%u/%u\n", warmUs * 1000 / (kPageRounds * kPageKeys),
                found, static_cast<unsigned>(kPageRounds * kPageKeys));
  found = 0;
  t = esp_timer_get_time();
  for (uint32_t r = 0; r < 10000; ++r) found += store.find(gKeys[r % kBatched], nullptr) == Lookup::Found;
  Serial.printf("BENCH find_cold         per_lookup=%lldns  found=%u/10000\n", (esp_timer_get_time() - t) / 10, found);

  // Kerning pairs.
  t = esp_timer_get_time();
  unsigned pairsStored = 0;
  for (uint32_t p = 0; p < kPairs; ++p) {
    pairsStored +=
        store.storePair(0x41 + p % 26, 0x61 + p / 26, p % 2, static_cast<int16_t>(-static_cast<int>(p % 90)));
  }
  const int64_t pairWriteUs = esp_timer_get_time() - t;
  t = esp_timer_get_time();
  unsigned pairsRight = 0;
  for (uint32_t p = 0; p < kPairs; ++p) {
    int16_t kern = 1;
    if (store.findPair(0x41 + p % 26, 0x61 + p / 26, p % 2, &kern) && kern == -static_cast<int>(p % 90)) ++pairsRight;
  }
  Serial.printf("BENCH pair_store        per_pair=%lldus  stored=%u/%u\n", pairWriteUs / kPairs, pairsStored,
                static_cast<unsigned>(kPairs));
  Serial.printf("BENCH pair_find         per_pair=%lldns  right=%u/%u\n", (esp_timer_get_time() - t) * 1000 / kPairs,
                pairsRight, static_cast<unsigned>(kPairs));

  // A reboot: open scans the log to find where writing resumes.
  const uint32_t logUsed = store.logUsed();
  store.close();
  t = esp_timer_get_time();
  const bool reopened = store.open(kIdentity) == FlashGlyphStore::OpenResult::Ready;
  Serial.printf("BENCH open_scan         %u ms  log=%u B  slots=%u  resume point %s\n", msSince(t),
                static_cast<unsigned>(store.logUsed()), static_cast<unsigned>(store.usedSlots()),
                reopened && store.logUsed() == logUsed ? "matches" : "MISMATCH");
  Serial.printf("verify after reopen:    %u bad\n", static_cast<unsigned>(countBad(store)));

  // The wipe erases only the sectors that hold data, and never the header.
  t = esp_timer_get_time();
  const bool wiped = store.wipe();
  Serial.printf("BENCH wipe              %u ms  %s\n", msSince(t), wiped ? "ok" : "FAILED");
  uint8_t afterWipe = 0xFF;
  const bool sizeClaimed = store.sizeIndex(29 << 6, &afterWipe);
  Serial.printf("after wipe:             first glyph %s, sizes restart at %d\n",
                store.find(gKeys[0], nullptr) == Lookup::NotStored ? "absent" : "STILL THERE",
                sizeClaimed ? afterWipe : -1);

  // Leave no store header behind: the reader then sees no TTF store on the partition.
  store.close();
  flash.erase(0, NorFlash::kSectorBytes);
  Serial.printf("language slot untouched: %s\n", langSlotHash() == langBefore ? "yes" : "NO");
  finish(nullptr);
}

void loop() { delay(30000); }
