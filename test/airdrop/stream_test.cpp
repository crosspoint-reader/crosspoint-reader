#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

#include "../../vendor/esp32drop/src/airdrop/core/ad_cpio_stream.h"

struct Target {
  uint64_t bytes = 0;
  uint32_t begins = 0, ends = 0;
  bool accept = true;
  bool acceptEnd = true;
  AdCpioStream::Sink sink() {
    return {this,
            [](void* v, const char*, uint32_t) {
              auto& t = *static_cast<Target*>(v);
              ++t.begins;
              return t.accept;
            },
            [](void* v, const uint8_t*, uint32_t n) {
              auto& t = *static_cast<Target*>(v);
              t.bytes += n;
              return t.accept;
            },
            [](void* v) {
              auto& t = *static_cast<Target*>(v);
              ++t.ends;
              return t.acceptEnd;
            }};
  }
};
static std::string entry(const char* name, uint32_t size, bool modern = false) {
  char h[128];
  if (!modern)
    snprintf(h, sizeof(h), "070707%06o%06o%06o%06o%06o%06o%06o%011o%06o%011o", 0, 0, 0100644, 0, 0, 1, 0, 0,
             (unsigned)strlen(name) + 1, size);
  else
    snprintf(h, sizeof(h), "070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x", 0, 0100644, 0, 0, 1, 0, size,
             0, 0, 0, 0, (unsigned)strlen(name) + 1, 0);
  std::string r = h;
  r.append(name, strlen(name) + 1);
  if (modern)
    while (r.size() % 4) r.push_back(0);
  return r;
}
static bool feed(AdCpioStream& p, const std::string& s, unsigned grain) {
  for (size_t off = 0; off < s.size(); off += grain)
    if (!p.feed(reinterpret_cast<const uint8_t*>(s.data() + off), std::min<size_t>(grain, s.size() - off)))
      return false;
  return true;
}
int main() {
  constexpr uint32_t cap = 30u * 1024 * 1024;
  for (bool modern : {false, true})
    for (unsigned grain : {1u, 3u, 76u, 110u, 4096u}) {
      Target t;
      AdCpioStream p;
      p.reset(t.sink(), cap);
      std::string body = entry("./book.epub", 17, modern) + std::string(17, 'x');
      if (modern)
        while (body.size() % 4) body.push_back(0);
      body += entry("empty", 0, modern);
      body += entry("TRAILER!!!", 0, modern);
      body.append(128, 0);
      assert(feed(p, body, grain));
      assert(p.complete());
      assert(t.bytes == 17 && t.ends == 2);
      body.pop_back();  // Padding is optional after the trailer.
      Target u;
      AdCpioStream q;
      q.reset(u.sink(), cap);
      assert(feed(q, body, grain));
      assert(q.complete());
    }
  // Exactly 30 MiB decoded, with a single file larger than device PSRAM.
  auto head = entry("large.epub", 0), tail = entry("TRAILER!!!", 0);
  uint32_t payload = cap - head.size() - tail.size();
  head = entry("large.epub", payload);
  Target t;
  AdCpioStream p;
  p.reset(t.sink(), cap);
  assert(feed(p, head, 7));
  std::vector<uint8_t> chunk(131072, 42);
  for (uint32_t left = payload; left;) {
    auto n = std::min<uint32_t>(left, chunk.size());
    assert(p.feed(chunk.data(), n));
    left -= n;
  }
  assert(feed(p, tail, 13));
  assert(p.complete() && t.bytes == payload && t.ends == 1);
  uint8_t zero = 0;
  assert(!p.feed(&zero, 1));
  assert(!p.complete());
  // Every cut before the trailer must remain incomplete.
  std::string body = entry("x", 9) + std::string(9, 'x') + entry("TRAILER!!!", 0);
  for (size_t cut = 0; cut < body.size(); ++cut) {
    Target u;
    AdCpioStream q;
    q.reset(u.sink(), cap);
    feed(q, body.substr(0, cut), 1);
    assert(!q.complete());
  }
  for (auto bad :
       {std::string("garbage"), entry("x", UINT32_MAX), entry("TRAILER!!!", 1), entry("TRAILER!!!", 0) + "junk"}) {
    Target u;
    AdCpioStream q;
    q.reset(u.sink(), cap);
    assert(!feed(q, bad, 1));
  }
  // Header overflow in odc's 11-digit filesize field must not wrap.
  auto overflow = entry("x", 0);
  overflow.replace(65, 11, "77777777777");
  Target u;
  AdCpioStream q;
  q.reset(u.sink(), cap);
  assert(!feed(q, overflow, 1));
  // Storage refuses begin, write, and close separately.
  for (int stage = 0; stage < 3; ++stage) {
    Target u;
    AdCpioStream q;
    q.reset(u.sink(), cap);
    u.accept = stage != 0;
    bool ok = feed(q, entry("x", 1), 1);
    if (stage == 0) {
      assert(!ok);
      continue;
    }
    u.accept = stage != 1;
    const uint8_t byte = 42;
    if (stage == 2) u.acceptEnd = false;
    assert(!q.feed(&byte, 1));
    assert(!q.complete());
  }
  puts(
      "stream parser: chunk boundaries, exact 30 MiB, over-limit, truncation, malformed headers and storage failures "
      "passed");
}
