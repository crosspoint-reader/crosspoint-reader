#include <CommonCrypto/CommonDigest.h>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

#include "DfuSession.h"
#include "OtaBootSwitch.h"
#include "UsbDfuUpdate.cpp"
#include "UsbFlashWriter.h"
using namespace usb_update;
namespace usb_update {
void suspendCache() {}
bool resumeCache() { return true; }
Report run(Session&, bool (*)(), uint32_t, uint32_t, bool, void (*)(bool)) { return {}; }
}  // namespace usb_update
std::vector<unsigned char> source;
bool sourceReadFailure = false;
static std::vector<uint8_t> flash(0x640000);
#if FREEINK_DEVICE_X4PRO
static constexpr uint16_t CHIP = 9;
static constexpr const char* BOARD = "x4pro";
#else
static constexpr uint16_t CHIP = 5;
static constexpr const char* BOARD = "x4";
#endif
static esp_partition_t running{0x10000, 0x640000, "recovery"}, dest{0x650000, 0x640000, "application"};
static bool eraseFailure = false, writeFailure = false, readFailure = false, selectionFailure = false;
static unsigned selections = 0, writes = 0, erases = 0;
const esp_partition_t* esp_ota_get_running_partition() { return &running; }
const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t*) { return &dest; }
int esp_partition_erase_range(const esp_partition_t* p, size_t offset, size_t n) {
  assert(p == &dest && offset + n <= flash.size() && !(offset % 4096) && !(n % 4096));
  erases++;
  if (eraseFailure) return -1;
  memset(flash.data() + offset, 255, n);
  return 0;
}
int esp_partition_write(const esp_partition_t* p, size_t offset, const void* data, size_t n) {
  assert(p == &dest && offset + n <= flash.size());
  writes++;
  if (writeFailure) return -1;
  memcpy(flash.data() + offset, data, n);
  return 0;
}
int esp_partition_read(const esp_partition_t* p, size_t offset, void* data, size_t n) {
  if (p == &running) {
    assert(offset == 12 && n == 2);
    uint16_t chip = CHIP;
    memcpy(data, &chip, 2);
    return 0;
  }
  assert(p == &dest && offset + n <= flash.size());
  if (readFailure) return -1;
  memcpy(data, flash.data() + offset, n);
  return 0;
}
namespace ota_boot {
bool switchTo(const esp_partition_t* p) {
  assert(p == &dest);
  if (selectionFailure) return false;
  selections++;
  return true;
}
}  // namespace ota_boot
static void reset() {
  eraseFailure = writeFailure = readFailure = selectionFailure = false;
  selections = writes = erases = 0;
  std::fill(flash.begin(), flash.end(), 0xa5);
}
static void hash(const std::vector<uint8_t>& data, uint8_t sha[32]) {
  CC_SHA256(data.data(), CC_LONG(data.size()), sha);
}
static void word(std::vector<uint8_t>& p, size_t at, uint32_t n) {
  for (unsigned i = 0; i < 4; i++) p[at + i] = n >> (8 * i);
}
static std::vector<uint8_t> image(const char* tag = BOARD, uint16_t chip = CHIP) {
  std::vector<uint8_t> b(24 + 8 + 65536);
  b[0] = 0xe9;
  b[1] = 1;
  b[12] = chip;
  b[13] = chip >> 8;
  b[23] = 1;
  word(b, 28, 65536);
  uint32_t rng = 12345;
  for (size_t i = 32; i < b.size(); i++) {
    rng = rng * 1664525u + 1013904223u;
    b[i] = rng >> 24;
  }
  const std::string t = std::string("CROSSPOINT-BOARD-V1:") + tag + ";";
  memcpy(b.data() + 100, t.data(), t.size());
  uint8_t sum = 0xef;
  for (size_t i = 32; i < b.size(); i++) sum ^= b[i];
  b.resize((b.size() + 16) & ~size_t(15));
  b.back() = sum;
  uint8_t sha[32];
  hash(b, sha);
  b.insert(b.end(), sha, sha + 32);
  return b;
}
static bool installRaw(const std::vector<uint8_t>& b, firmware_flash::Result expected = firmware_flash::Result::OK) {
  uint8_t sha[32];
  hash(b, sha);
  FlashWriter writer;
  assert(writer.begin(&dest, b.size(), sha));
  for (size_t i = 0; i < b.size(); i += 32760) {
    if (!writer.append(b.data() + i, std::min<size_t>(32760, b.size() - i))) return false;
  }
  const bool ok = writer.finish();
  assert(writer.result == expected);
  return ok;
}
static std::vector<uint8_t> load(const char* path) {
  std::ifstream f(path, std::ios::binary);
  assert(f);
  return {std::istreambuf_iterator<char>(f), {}};
}
int main(int argc, char** argv) {
  using R = firmware_flash::Result;
  auto valid = image();
  reset();
  assert(installRaw(valid));
  assert(selections == 1 && writes == 3 && erases == 2);
  reset();
  assert(!installRaw(image("sticky"), R::WRONG_BOARD) && !selections);
  reset();
  assert(!installRaw(image(BOARD, CHIP == 5 ? 9 : 5), R::BAD_CHIP) && !selections);
  auto corrupt = valid;
  word(corrupt, 28, 0xffffffff);
  reset();
  assert(!installRaw(corrupt, R::BAD_SEGMENTS) && !selections);
  corrupt = valid;
  corrupt[300] ^= 1;
  reset();
  assert(!installRaw(corrupt, R::BAD_CHECKSUM) && !selections);
  corrupt = valid;
  corrupt.back() ^= 1;
  reset();
  assert(!installRaw(corrupt, R::BAD_SHA) && !selections);
  reset();
  readFailure = true;
  assert(!installRaw(valid, R::READ_FAIL) && !selections);
  reset();
  selectionFailure = true;
  assert(!installRaw(valid, R::OTADATA_FAIL) && !selections);
  reset();
  eraseFailure = true;
  assert(!installRaw(valid) && !selections);
  reset();
  writeFailure = true;
  assert(!installRaw(valid) && !selections);
  reset();
  uint8_t sha[32];
  hash(valid, sha);
  FlashWriter guarded;
  assert(!guarded.begin(&running, valid.size(), sha) && !writes && !erases);
  reset();
  FlashWriter partial;
  assert(partial.begin(&dest, valid.size(), sha));
  assert(partial.append(valid.data(), 32760));
  assert(!partial.finish() && !selections);
  partial.abort();
  assert(!partial.append(valid.data(), 1) && !selections);
  // Mutate stored bytes after the writes: both embedded verification and the
  // host's whole-image digest are checked against actual flash readback.
  for (size_t offset : {size_t(0), size_t(100), valid.size() - 1}) {
    reset();
    FlashWriter w;
    assert(w.begin(&dest, valid.size(), sha));
    for (size_t i = 0; i < valid.size(); i += 32760)
      assert(w.append(valid.data() + i, std::min<size_t>(32760, valid.size() - i)));
    flash[offset] ^= 1;
    assert(!w.finish() && !selections);
  }
  reset();
  sha[0] ^= 1;
  FlashWriter wrongHash;
  assert(wrongHash.begin(&dest, valid.size(), sha));
  for (size_t i = 0; i < valid.size(); i += 32760)
    assert(wrongHash.append(valid.data() + i, std::min<size_t>(32760, valid.size() - i)));
  assert(!wrongHash.finish() && wrongHash.result == R::READBACK_FAIL && !selections);

  // Exercise the production low-memory sink, including partial-block buffering,
  // cancellation/restart, compressed-header rejection, and raw DFU manifestation.
  for (unsigned attempt = 0; attempt < 4; attempt++) {
    reset();
    usb_dfu::FlashSink sink(nullptr, nullptr);
    Session session(sink, 0x640000, 65536, 10000, 1000, 4096);
    std::vector<uint8_t> h(64);
    memcpy(h.data(), "X3DFU001", 8);
    word(h, 8, valid.size());
    hash(valid, h.data() + 12);
    assert(session.download(0, h.data(), 64) == Error::None);
    assert(session.download(1, valid.data(), 4096) == Error::None);
    assert(writes == 0);
    session.abort();
    assert(!selections);
    assert(session.download(0, h.data(), 64) == Error::None);
    for (size_t off = 0; off < valid.size(); off += 4096)
      assert(session.download(1 + off / 4096, valid.data() + off, std::min<size_t>(4096, valid.size() - off)) ==
             Error::None);
    assert(session.manifest() == Error::None && selections == 1 && session.complete());
    assert(std::equal(valid.begin(), valid.end(), flash.begin()));
    session.abort();
    assert(session.complete());
  }
  reset();
  {
    usb_dfu::FlashSink sink(nullptr, nullptr);
    Session s(sink, 0x640000, 65536, 10000, 1000, 4096);
    std::vector<uint8_t> h(64);
    memcpy(h.data(), "X3DFU002", 8);
    word(h, 8, 73728);
    word(h, 44, valid.size());
    hash(valid, h.data() + 12);
    assert(s.download(0, h.data(), 64) == Error::Write && !writes && !erases && !selections);
  }
  reset();
  {
    struct Events {
      unsigned prepareCalls = 0, waiting = 0;
      bool allow = false;
      std::vector<size_t> positions;
    } events;
    auto prep = [](void* p) {
      auto& e = *static_cast<Events*>(p);
      ++e.prepareCalls;
      return e.allow;
    };
    auto progress = [](usb_dfu::Stage stage, size_t count, size_t, void* p) {
      auto& e = *static_cast<Events*>(p);
      if (stage == usb_dfu::Stage::Waiting) ++e.waiting;
      if (stage == usb_dfu::Stage::Receiving) e.positions.push_back(count);
    };
    usb_dfu::FlashSink sink(progress, &events, prep);
    Session s(sink, 0x640000, 65536, 10000, 1000, 4096);
    std::vector<uint8_t> h(64);
    assert(s.pollTimeout(false) == 8000 && !events.prepareCalls);
    s.abort();
    assert(!events.prepareCalls && events.positions.empty());
    assert(s.download(0, h.data(), h.size()) == Error::File && !events.prepareCalls);
    s.abort();
    memcpy(h.data(), "X3DFU001", 8);
    word(h, 8, 1024 * 1024);
    assert(s.download(0, h.data(), h.size()) == Error::Write);
    assert(events.prepareCalls == 1 && events.waiting == 1 && !writes && !erases);
    s.abort();
    events.allow = true;
    assert(s.download(0, h.data(), h.size()) == Error::None);
    assert(events.positions.size() == 1 && events.positions[0] == 0 && !writes);
    std::vector<uint8_t> payload(4096, 0xa5);
    for (unsigned block = 1; block <= 16; ++block) {
      testMillis += 100;
      const unsigned wait = s.pollTimeout(false);
      if (block == 12) assert(wait < 100);  // Only flash work, no display budget.
      assert(s.download(block, payload.data(), payload.size()) == Error::None);
    }
    assert(events.positions.size() == 1 && events.positions[0] == 0);  // No paints while receiving.
    s.abort();
    assert(events.waiting == 2 && !selections);
    auto slowProgress = [](usb_dfu::Stage stage, size_t count, size_t, void* p) {
      auto& e = *static_cast<Events*>(p);
      if (stage == usb_dfu::Stage::Receiving) e.positions.push_back(count);
      testMillis += 300;  // Blocking refresh fallback, e.g. sunlight fading fix.
    };
    events.positions.clear();
    usb_dfu::FlashSink slow(slowProgress, &events);
    assert(slow.begin(1024 * 1024, h.data()));
    for (unsigned block = 1; block <= 256; ++block) {
      testMillis += 100;
      const auto wait = slow.pollTimeout(false, (block - 1) * 4096, 1024 * 1024, 4096);
      if (block == 32 || block == 256) assert(wait < 100);
      assert(slow.append(payload.data(), payload.size()));
      assert(events.positions.size() == 1);
    }
    assert(events.positions.size() == 1 && events.positions[0] == 0);
    assert(slow.uiCount() == 1 && slow.uiTimeMs() == 300 && slow.uiMaxMs() == 300);
    assert(slow.finish() == Error::Verify);  // Invalid payload reaches verification without an extra 100% paint.
    assert(slow.uiCount() == 2 && slow.uiTimeMs() == 600);
    slow.abort();
  }
  assert(argc == 2);
  const auto real = load(argv[1]);
  reset();
  assert(installRaw(real));
  assert(selections == 1);
  assert(std::equal(real.begin(), real.end(), flash.begin()));
  printf(
      "PASS: inactive-slot writer and production CrossPoint validator; corruption, truncation, wrong chip/board/hash "
      "and storage failures reject without boot selection. Real image %zu bytes.\n",
      real.size());
}
