#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "network/ProtectedPaths.h"
#include "util/PluginEventAsset.h"
#include "util/PluginEvents.h"
#include "util/PluginHttp.h"

class GfxRenderer {};
struct GuiStub {
  void drawPopup(GfxRenderer&, const char*) {}
};
inline GuiStub GUI;

// Production HTTP vocabulary helpers; only transport/token I/O are stubbed.
#include "ProductionHttp.inc"
namespace pluginhttp {
int replyStatus = 200, sends = 0;
std::string replyBody;
int requestToFile(freeink::SecureHttpClient*, const std::string&, const std::string&, const std::string&,
                  const Headers&, const char* dest, size_t cap) {
  ++sends;
  if (replyBody.size() > cap) return -1;
  Storage.files[dest] = replyBody;
  return replyStatus;
}
int request(freeink::SecureHttpClient*, const std::string&, const std::string&, const std::string&, const Headers&,
            String&, size_t, Headers*, const std::function<bool()>&) {
  ++sends;
  return replyStatus;
}
bool loadTokenFromFile(const std::string&, const std::string&, std::string&) { return true; }
bool saveTokenToFile(const std::string&, const std::string&, const std::string&) { return true; }
void loadConfigFile(const std::string&, Headers&) {}
bool mintPasswordToken(freeink::SecureHttpClient*, const std::string&, const std::string&, const std::string&,
                       const Headers&, const std::string&, std::string&) {
  return false;
}
}  // namespace pluginhttp

// Unmodified production manifest parser, delivery and queue drain.
#include "ProductionDrain.inc"

namespace {
void le16(std::string& s, size_t at, uint16_t v) {
  s[at] = static_cast<char>(v);
  s[at + 1] = static_cast<char>(v >> 8);
}
void le32(std::string& s, size_t at, uint32_t v) {
  for (size_t i = 0; i < 4; ++i) s[at + i] = static_cast<char>(v >> (i * 8));
}
std::string bmp(unsigned char color = 0) {
  // 2x2, 24-bit BI_RGB; each row is 8 bytes including padding.
  std::string s(70, '\0');
  for (size_t i = 54; i < s.size(); ++i) s[i] = static_cast<char>(color);
  le16(s, 0, 0x4d42);
  le32(s, 2, 70);
  le32(s, 10, 54);
  le32(s, 14, 40);
  le32(s, 18, 2);
  le32(s, 22, 2);
  le16(s, 26, 1);
  le16(s, 28, 24);
  le32(s, 30, 0);
  le32(s, 34, 16);
  return s;
}
void reset() { Storage = StorageStub{}; }
}  // namespace

void helperChecks() {
  // Destination insertion can rehash the map; rename must erase by key.
  reset();
  Storage.files["/from"] = "old";
  Storage.files.max_load_factor(Storage.files.load_factor());
  const size_t buckets = Storage.files.bucket_count();
  assert(Storage.rename("/from", "/to"));
  assert(Storage.files.bucket_count() > buckets);
  assert(!Storage.exists("/from") && Storage.files["/to"] == "old");
  const std::string old = bmp(1), fresh = bmp(2);
  reset();
  Storage.files["/sleep.bmp"] = old;
  Storage.files["/sleep.bmp.part"] = fresh;
  assert(plugineventasset::validBmp("/sleep.bmp.part", 2, 2));
  assert(!plugineventasset::validBmp("/sleep.bmp.part", 3, 2));
  assert(plugineventasset::commit("/sleep.bmp", 2, 2, 200));
  assert(Storage.files["/sleep.bmp"] == fresh && !Storage.exists("/sleep.bmp.bak"));

  // A complete-looking header with missing pixel rows must never replace old.
  for (const std::string bad :
       {std::string("<html>bad</html>"), fresh.substr(0, 60), std::string(1024 * 1024 + 1, 'x')}) {
    reset();
    Storage.files["/sleep.bmp"] = old;
    Storage.files["/sleep.bmp.part"] = bad;
    assert(!plugineventasset::commit("/sleep.bmp", 2, 2, 200));
    assert(Storage.files["/sleep.bmp"] == old);
  }
  // Parser edge cases must be rejected before Bitmap reads beyond the DIB.
  for (const int edge : {0, 1, 2, 3, 4}) {
    std::string bad = fresh;
    if (edge == 0) le32(bad, 22, 0x80000000u);   // negating INT32_MIN overflows
    if (edge == 1) le32(bad, 10, 54 + 256 * 4);  // pixel offset beyond file
    if (edge == 2) le32(bad, 14, 124);           // extended DIB not parsed by this caller
    if (edge == 3) le32(bad, 2, 69);             // declared BMP size differs from bytes
    if (edge == 4) {
      le16(bad, 28, 32);
      le32(bad, 30, 3);  // BI_BITFIELDS without parsed masks
    }
    reset();
    Storage.files["/sleep.bmp.part"] = bad;
    assert(!plugineventasset::validBmp("/sleep.bmp.part", 2, 2));
  }
  reset();
  Storage.files["/sleep.bmp"] = old;
  Storage.files["/sleep.bmp.part"] = fresh;
  for (const int status : {204, 206, 404, -1}) {
    assert(!plugineventasset::commit("/sleep.bmp", 2, 2, status));
    assert(Storage.files["/sleep.bmp"] == old);
  }

  // Failing either rename keeps last-good, at dest or at its recoverable bak.
  reset();
  Storage.files["/sleep.bmp"] = old;
  Storage.files["/sleep.bmp.part"] = fresh;
  Storage.failRenameFrom = "/sleep.bmp";
  assert(!plugineventasset::commit("/sleep.bmp", 2, 2, 200));
  assert(Storage.files["/sleep.bmp"] == old);

  reset();
  Storage.files["/sleep.bmp"] = old;
  Storage.files["/sleep.bmp.part"] = fresh;
  Storage.failRenameFrom = "/sleep.bmp.part";
  assert(!plugineventasset::commit("/sleep.bmp", 2, 2, 200));
  assert(Storage.files["/sleep.bmp"] == old);

  reset();
  Storage.files["/sleep.bmp"] = old;
  Storage.files["/sleep.bmp.part"] = fresh;
  Storage.failRenameFrom = "/sleep.bmp.part";
  Storage.failRenameTo = "/sleep.bmp";
  assert(!plugineventasset::commit("/sleep.bmp", 2, 2, 200));
  assert(Storage.files["/sleep.bmp.bak"] == old);
  Storage.failRenameFrom.clear();
  Storage.failRenameTo.clear();
  assert(plugineventasset::recover("/sleep.bmp"));
  assert(Storage.files["/sleep.bmp"] == old);

  // Reset states: backup only, valid dest + stale backup, corrupt dest + good backup.
  reset();
  Storage.files["/sleep.bmp.bak"] = old;
  Storage.files["/sleep.bmp.part"] = "bad";
  assert(plugineventasset::recover("/sleep.bmp"));
  assert(Storage.files["/sleep.bmp"] == old);
  reset();
  Storage.files["/sleep.bmp"] = fresh;
  Storage.files["/sleep.bmp.bak"] = old;
  assert(plugineventasset::recover("/sleep.bmp"));
  assert(Storage.files["/sleep.bmp"] == fresh && !Storage.exists("/sleep.bmp.bak"));
  reset();
  Storage.files["/sleep.bmp"] = "bad";
  Storage.files["/sleep.bmp.bak"] = old;
  assert(plugineventasset::recover("/sleep.bmp"));
  assert(Storage.files["/sleep.bmp"] == old);

  // A failed SD removal must not discard the only valid backup.
  reset();
  Storage.files["/sleep.bmp"] = "bad";
  Storage.files["/sleep.bmp.bak"] = old;
  Storage.failRemove = "/sleep.bmp";
  assert(!plugineventasset::recover("/sleep.bmp"));
  assert(Storage.files["/sleep.bmp.bak"] == old);
}

namespace {
const std::string queued = R"({"e":"sleep.enter","id":"test","ts":1,"vars":{}})"
                           "\n";
const char* queuePath = "/plugins/fixture/events.jsonl";
void setupDownload(const std::string& fields, int status, const std::string& body) {
  reset();
  for (auto& sub : subscribers) sub = Subscriber{};
  strcpy(subscribers[0].name, "fixture");
  strcpy(subscribers[0].dir, "/plugins/fixture");
  Storage.files["/plugins/fixture/device.json"] =
      R"({"events":{"sleep.enter":{"download":{"url":"https://fixture.invalid/frame","dest":"/sleep.bmp")" + fields +
      "}}}}";
  Storage.files[queuePath] = queued;
  Storage.files["/sleep.bmp"] = bmp(1);
  pluginhttp::sends = 0;
  pluginhttp::replyStatus = status;
  pluginhttp::replyBody = body;
}
void expectQueued(const std::string& fields, int status, const std::string& body, int sends) {
  setupDownload(fields, status, body);
  pluginevents::drain(nullptr, 4);
  assert(Storage.files[queuePath] == queued);
  assert(Storage.files["/sleep.bmp"] == bmp(1));
  assert(pluginhttp::sends == sends);
  assert(!Storage.exists("/sleep.bmp.part"));
}
}  // namespace

int main() {
  helperChecks();
  const std::string checked = R"(,"format":"bmp","width":2,"height":2)";
  for (const std::string format : {"null", "true", "42", "{}", "[]", "\"\"", "\"png\""}) {
    expectQueued(",\"format\":" + format + ",\"width\":2,\"height\":2", 200, bmp(2), 0);
  }
  for (const std::string value : {"null", "true", "\"2\"", "2.5", "0", "-2", "2147483648", "{}", "[]"}) {
    expectQueued(",\"format\":\"bmp\",\"width\":" + value + ",\"height\":2", 200, bmp(2), 0);
    expectQueued(",\"format\":\"bmp\",\"width\":2,\"height\":" + value, 200, bmp(2), 0);
  }
  for (const std::string fields : {R"(,"format":"bmp")", R"(,"format":"bmp","width":2049,"height":2)",
                                   R"(,"format":"bmp","width":2,"height":3073)"}) {
    expectQueued(fields, 200, bmp(2), 0);
  }
  expectQueued(checked, 200, "<html>error</html>", 1);
  expectQueued(checked, 200, bmp(2).substr(0, 60), 1);
  for (int status : {204, 206, 304, 401, 500, -1}) expectQueued(checked, status, bmp(2), 1);
  expectQueued(R"(,"format":"bmp","width":3,"height":2)", 200, bmp(2), 1);

  setupDownload(checked, 200, bmp(2));
  pluginevents::drain(nullptr, 4);
  assert(!Storage.exists(queuePath) && Storage.files["/sleep.bmp"] == bmp(2));
  assert(pluginhttp::sends == 1);

  // No format means legacy generic, even with unrelated malformed dimensions.
  for (int status : {200, 204, 206, 299}) {
    setupDownload(R"(,"width":"bad","height":false)", status, "generic body");
    pluginevents::drain(nullptr, 4);
    assert(!Storage.exists(queuePath) && Storage.files["/sleep.bmp"] == "generic body");
  }
  expectQueued("", 500, "error", 1);

  // Queue order and delivered-prefix rewrite use the actual drain.
  setupDownload(checked, 200, bmp(2));
  Storage.files[queuePath] += queued;
  pluginevents::drain(nullptr, 1);
  assert(Storage.files[queuePath] == queued && pluginhttp::sends == 1);

  // Backup-only reset is recovered before an unsuccessful request.
  setupDownload(checked, 500, "error");
  Storage.files.erase("/sleep.bmp");
  Storage.files["/sleep.bmp.bak"] = bmp(1);
  pluginevents::drain(nullptr, 4);
  assert(Storage.files["/sleep.bmp"] == bmp(1) && !Storage.exists("/sleep.bmp.bak"));
  assert(Storage.files[queuePath] == queued);

  // Failed park, failed install, and failed rollback preserve last-good.
  for (int fault : {0, 1, 2}) {
    setupDownload(checked, 200, bmp(2));
    Storage.failRenameFrom = fault == 0 ? "/sleep.bmp" : "/sleep.bmp.part";
    if (fault == 2) Storage.failRenameTo = "/sleep.bmp";
    pluginevents::drain(nullptr, 4);
    assert(Storage.files[queuePath] == queued);
    assert(!Storage.exists("/sleep.bmp.part"));
    assert(Storage.files[fault == 2 ? "/sleep.bmp.bak" : "/sleep.bmp"] == bmp(1));
    Storage.failRenameFrom.clear();
    Storage.failRenameTo.clear();
    pluginhttp::replyStatus = 500;
    pluginevents::drain(nullptr, 4);
    assert(Storage.files["/sleep.bmp"] == bmp(1));
  }
  // Failed stale-backup removal prevents a request and preserves both images.
  setupDownload(checked, 200, bmp(2));
  Storage.files["/sleep.bmp.bak"] = bmp(3);
  Storage.failRemove = "/sleep.bmp.bak";
  pluginevents::drain(nullptr, 4);
  assert(pluginhttp::sends == 0 && Storage.files["/sleep.bmp"] == bmp(1));
  assert(Storage.files["/sleep.bmp.bak"] == bmp(3) && Storage.files[queuePath] == queued);

  // Failed final cleanup leaves two usable files; next drain cleans safely.
  setupDownload(checked, 200, bmp(2));
  Storage.failRemove = "/sleep.bmp.bak";
  pluginevents::drain(nullptr, 4);
  assert(!Storage.exists(queuePath) && Storage.files["/sleep.bmp"] == bmp(2));
  assert(Storage.files["/sleep.bmp.bak"] == bmp(1));
  Storage.failRemove.clear();
  Storage.files[queuePath] = queued;
  pluginhttp::replyStatus = 500;
  pluginevents::drain(nullptr, 4);
  assert(!Storage.exists("/sleep.bmp.bak") && Storage.files["/sleep.bmp"] == bmp(2));

  // Removing an invalid handler permits the existing queue cleanup.
  setupDownload(",\"format\":null", 200, bmp(2));
  Storage.files["/plugins/fixture/device.json"] = "{}";
  pluginevents::drain(nullptr, 4);
  assert(!Storage.exists(queuePath) && pluginhttp::sends == 0);
}
