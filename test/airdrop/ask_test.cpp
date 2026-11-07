#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../../vendor/esp32drop/src/airdrop/core/ad_ask.h"
#include "../../vendor/esp32drop/src/airdrop/core/ad_serve.h"
struct Source {
  std::string wire;
  size_t pos = 0;
  uint32_t now = 0;
  bool stop = false;
  uint32_t drip = 0;
  size_t maxRead = 65536, stopAt = 0;
};
static int readBytes(HttpRdr* r, unsigned char* out, int cap) {
  auto* s = (Source*)r->ctx;
  if (s->pos == s->wire.size()) return -1;
  auto n = std::min<size_t>(s->maxRead, std::min<size_t>(cap, s->wire.size() - s->pos));
  s->now += s->drip;
  memcpy(out, s->wire.data() + s->pos, n);
  s->pos += n;
  if (s->stopAt && s->pos >= s->stopAt) s->stop = true;
  return n;
}
static uint32_t now(HttpServe* s) { return ((Source*)s->io)->now++; }
static bool stop(HttpServe* s) { return ((Source*)s->io)->stop; }
static void timeout(HttpRdr*, uint32_t) {}
static void framing() {
  for (int mode = 0; mode < 4; ++mode) {
    Source source{std::string(5000, 'a')};
    source.maxRead = 1;
    source.drip = mode == 0 || mode == 1 ? 1000 : 0;
    source.stopAt = mode == 3 ? 3 : 0;
    HttpRdr r;
    http_rdr_init(&r, &source, readBytes);
    HttpServe sv{};
    sv.r = &r;
    sv.io = &source;
    sv.now = now;
    sv.set_timeout = timeout;
    sv.cancelled = stop;
    uint8_t out[5000];
    uint32_t n;
    assert(!http_read_ask(&sv, mode == 1 || mode == 2, 5000, out, sizeof(out), &n));
    assert(source.now <= 22000 && source.pos <= 40);
  }

  for (bool chunked : {false, true}) {
    Source source{chunked ? "3\r\nabc\r\n2\r\nde\r\n0\r\n\r\n" : "abcde"};
    HttpRdr r;
    http_rdr_init(&r, &source, readBytes);
    HttpServe sv{};
    sv.r = &r;
    sv.io = &source;
    sv.now = now;
    sv.set_timeout = timeout;
    sv.cancelled = stop;
    uint8_t out[5];
    uint32_t n;
    assert(http_read_ask(&sv, chunked, 5, out, 5, &n));
    assert(n == 5 && !memcmp(out, "abcde", 5));
  }
  for (auto wire : {"x\r\n", "100000000\r\n", "6\r\nabcdef\r\n0\r\n\r\n", "3\r\nab", "3\r\nabcxx", "0\r\nx\n"}) {
    Source source{wire};
    HttpRdr r;
    http_rdr_init(&r, &source, readBytes);
    HttpServe sv{};
    sv.r = &r;
    sv.io = &source;
    sv.now = now;
    sv.set_timeout = timeout;
    uint8_t out[5];
    uint32_t n;
    assert(!http_read_ask(&sv, true, 0, out, 5, &n));
  }
  Source source{"abcde"};
  source.stop = true;
  HttpRdr r;
  http_rdr_init(&r, &source, readBytes);
  HttpServe sv{};
  sv.r = &r;
  sv.io = &source;
  sv.now = now;
  sv.set_timeout = timeout;
  sv.cancelled = stop;
  uint8_t out[5];
  uint32_t n;
  assert(!http_read_ask(&sv, false, 5, out, 5, &n));
}
int main(int argc, char** argv) {
  framing();
  if (argc != 2) return 1;
  std::ifstream in(argv[1], std::ios::binary);
  std::vector<uint8_t> b{std::istreambuf_iterator<char>(in), {}};
  AdAskParser p;
  AdAskInfo info;
  bool valid = p.parse(b.data(), b.size(), info);
  // Exercise every truncated trailer and single-byte mutations under sanitizers.
  for (size_t cut = 0; cut < b.size(); ++cut) {
    AdAskInfo scratch;
    p.parse(b.data(), cut, scratch);
  }
  for (size_t i = 0; i < b.size(); ++i) {
    b[i] ^= 0xff;
    AdAskInfo scratch;
    p.parse(b.data(), b.size(), scratch);
    b[i] ^= 0xff;
  }
  if (!valid) return 2;
  printf("%s\n%u %llu %d\n", info.filename, info.fileCount, (unsigned long long)info.totalBytes, info.sizesKnown);
  return 0;
}
