#include <HalStorage.h>
#include <SecureHttpClient.h>

#include <cassert>
#include <string>
#include <vector>

#include "util/PluginEvents.h"
#include "util/PluginHttp.h"
#include "util/PluginLocations.h"

uint32_t millis() { return freeink::fakeNow; }
namespace PluginLocations {
std::vector<Entry> entries;
std::vector<Entry> scanPlugins() { return entries; }
std::string findPluginDir(const char*) { return {}; }
}  // namespace PluginLocations
static void setup(const std::vector<std::string>& names, const std::string& manifest, const std::string& outbox) {
  Storage.files.clear();
  PluginLocations::entries.clear();
  freeink::fakeNow = 0;
  freeink::SecureHttpClient::sends = 0;
  freeink::SecureHttpClient::sent.clear();
  freeink::SecureHttpClient::beginDelayMs = 0;
  freeink::SecureHttpClient::aborted = false;
  for (const auto& name : names) {
    const std::string dir = "/plugins/" + name;
    PluginLocations::entries.push_back({name, dir, false, true, false});
    Storage.files[dir + "/device.json"] = manifest;
    Storage.files[dir + "/events.jsonl"] = outbox;
  }
  pluginevents::refreshSubscriptions();
}
int main() {
  using freeink::SecureHttpClient;
  const std::string line = R"({"e":"sleep.enter","id":"stable-1","ts":1,"vars":{}})"
                           "\n";
  const std::string second = R"({"e":"sleep.enter","id":"stable-2","ts":2,"vars":{}})"
                             "\n";
  const std::string manifest = R"({"events":{"sleep.enter":{"request":{"url":"https://example.test/frame"}}}})";
  // Failure consumes an attempt; the failed and following lines remain queued.
  setup({"a", "b"}, manifest, line + line);
  SecureHttpClient::replies = {{500, "", 1000}, {200, "", 1000}, {200, "", 1000}};
  pluginevents::drain(nullptr, 4, {3, 5000});
  assert(SecureHttpClient::sends == 3);
  assert(Storage.files["/plugins/a/events.jsonl"] == line + line);
  assert(!Storage.exists("/plugins/b/events.jsonl"));

  // Global three-attempt cap stops later plugins, even with success backlog.
  setup({"a", "b"}, manifest, line + line + line);
  SecureHttpClient::replies = {{200, "", 1000}, {200, "", 1000}, {200, "", 1000}, {200, "", 1000}};
  pluginevents::drain(nullptr, 4, {3, 5000});
  assert(SecureHttpClient::sends == 3);
  assert(!Storage.exists("/plugins/a/events.jsonl"));
  assert(Storage.files["/plugins/b/events.jsonl"] == line + line + line);

  // One 401, one auth mint, one retry use all three slots; later line remains.
  const std::string grant =
      R"({"token":{"file":"token.json"},"auth":{"type":"password","request":{"url":"https://example.test/auth"}},"events":{"sleep.enter":{"request":{"url":"https://example.test/frame"}}}})";
  setup({"a"}, grant, line + second);
  Storage.files["/plugins/a/token.json"] = R"({"token":"old"})";
  SecureHttpClient::replies = {
      {401, "", 1000}, {200, R"({"access_token":"new"})", 1000}, {200, "", 1000}, {200, "", 1000}};
  pluginevents::drain(nullptr, 4, {3, 5000});
  assert(SecureHttpClient::sends == 3);
  assert(Storage.files["/plugins/a/events.jsonl"] == second);
  assert(Storage.files["/plugins/a/token.json"] == R"({"token":"new"})");

  // Deadline during slow response leaves outbox; no auth or retry starts.
  setup({"a"}, grant, line);
  SecureHttpClient::replies = {{401, "", 5000}, {200, "", 0}};
  pluginevents::drain(nullptr, 4, {3, 5000});
  assert(SecureHttpClient::sends == 1);
  assert(Storage.files["/plugins/a/events.jsonl"] == line);

  // Attempt-only opt-in: time=0 does not disable the two-request cap.
  setup({"a"}, manifest, line + line + line);
  SecureHttpClient::replies = {{200, "", 1000}, {200, "", 1000}, {200, "", 1000}};
  pluginevents::drain(nullptr, 4, {2, 0});
  assert(SecureHttpClient::sends == 2);
  assert(Storage.files["/plugins/a/events.jsonl"] == line);

  // Deadline-only opt-in: attempts=0 is unlimited until the same clock expires.
  setup({"a"}, manifest, line + line + line);
  SecureHttpClient::replies = {{200, "", 2000}, {200, "", 3000}, {200, "", 0}};
  pluginevents::drain(nullptr, 4, {0, 5000});
  assert(SecureHttpClient::sends == 2);
  assert(Storage.files["/plugins/a/events.jsonl"] == line + line);

  // Legacy call has neither limit beyond its original delivered-line cap.
  setup({"a"}, manifest, line + line + line);
  SecureHttpClient::replies = {{200, "", 6000}, {200, "", 6000}, {200, "", 6000}};
  pluginevents::drain(nullptr, 4);
  assert(SecureHttpClient::sends == 3);
  assert(!Storage.exists("/plugins/a/events.jsonl"));

  // Successful prefix is removed; failed and later IDs remain in order.
  setup({"a"}, manifest, line + second + line);
  SecureHttpClient::replies = {{200, "", 1}, {500, "", 1}};
  pluginevents::drain(nullptr, 4, {4, 5000});
  assert(SecureHttpClient::sends == 2);
  assert(Storage.files["/plugins/a/events.jsonl"] == second + line);

  // Immediate transport failure costs a slot; an untouched later plugin keeps its queue.
  setup({"a", "b"}, manifest, line + second);
  SecureHttpClient::replies = {{-1, "", 0}, {200, "", 0}};
  pluginevents::drain(nullptr, 4, {1, 5000});
  assert(SecureHttpClient::sends == 1);
  assert(Storage.files["/plugins/a/events.jsonl"] == line + second);
  assert(Storage.files["/plugins/b/events.jsonl"] == line + second);

  // A deadline during mint prevents token persistence and a delivery retry.
  setup({"a"}, grant, line);
  Storage.files["/plugins/a/token.json"] = R"({"token":"old"})";
  SecureHttpClient::replies = {{401, "", 1000}, {200, R"({"access_token":"new"})", 4000}, {200, "", 0}};
  pluginevents::drain(nullptr, 4, {4, 5000});
  assert(SecureHttpClient::sends == 2 && Storage.files["/plugins/a/events.jsonl"] == line);
  assert(Storage.files["/plugins/a/token.json"] == R"({"token":"old"})");

  // Auth cannot mint without a slot, nor retry after the mint uses the last one.
  for (const size_t limit : {1, 2}) {
    setup({"a"}, grant, line);
    SecureHttpClient::replies = {{403, "", 1}, {200, R"({"access_token":"new"})", 1}, {200, "", 1}};
    pluginevents::drain(nullptr, 4, {limit, 5000});
    assert(SecureHttpClient::sends == limit);
    assert(Storage.files["/plugins/a/events.jsonl"] == line);
  }

  // Failure of the mint also consumes its slot; no delivery retry follows.
  setup({"a"}, grant, line);
  SecureHttpClient::replies = {{401, "", 1}, {500, "", 1}, {200, "", 1}};
  pluginevents::drain(nullptr, 4, {4, 5000});
  assert(SecureHttpClient::sends == 2 && Storage.files["/plugins/a/events.jsonl"] == line);

  // A completed server effect may miss its acknowledgement: replay keeps the ID.
  const std::string echo =
      R"({"events":{"sleep.enter":{"request":{"url":"https://example.test/{event.id}","body":"{event.id}"}}}})";
  setup({"a"}, echo, line);
  SecureHttpClient::replies = {{200, "", 5000, 0, false}};
  pluginevents::drain(nullptr, 4, {4, 5000});
  assert(Storage.files["/plugins/a/events.jsonl"] == line);
  SecureHttpClient::replies.push_back({200, "", 1});
  pluginevents::drain(nullptr, 4, {4, 5000});
  assert(SecureHttpClient::sent.size() == 2);
  assert(SecureHttpClient::sent[0].url == "https://example.test/stable-1");
  assert(SecureHttpClient::sent[1].body == SecureHttpClient::sent[0].body);
  assert(!Storage.exists("/plugins/a/events.jsonl"));

  // Both a mid-response cancel and a late cancel retain bytes and the queued ID.
  const std::string download =
      R"({"events":{"sleep.enter":{"download":{"url":"https://example.test/frame","dest":"/frame.dat"}}}})";
  for (const bool late : {false, true}) {
    setup({"a"}, download, line);
    Storage.files["/frame.dat"] = "previous bytes";
    SecureHttpClient::replies = {{200, "partial replacement", late ? 1U : 5000U, late ? 5000U : 0U, !late}};
    pluginevents::drain(nullptr, 4, {4, 5000});
    assert(Storage.files["/frame.dat"] == "previous bytes");
    assert(!Storage.exists("/frame.dat.part") && !Storage.exists("/frame.dat.bak"));
    assert(Storage.files["/plugins/a/events.jsonl"] == line);
  }
  // Successful download on the last slot keeps the existing generic commit policy.
  setup({"a"}, download, line);
  Storage.files["/frame.dat"] = "previous bytes";
  SecureHttpClient::replies = {{200, "replacement", 1}};
  pluginevents::drain(nullptr, 4, {1, 5000});
  assert(Storage.files["/frame.dat"] == "replacement");
  assert(!Storage.exists("/plugins/a/events.jsonl"));

  // The delivered-line cap is independent of unlimited attempt slots.
  setup({"a"}, manifest, line + second);
  SecureHttpClient::replies = {{200, "", 1}, {200, "", 1}};
  pluginevents::drain(nullptr, 1, {0, 0});
  assert(SecureHttpClient::sends == 1 && Storage.files["/plugins/a/events.jsonl"] == second);

  // The real drain's uint32_t elapsed calculation survives millis() wrapping.
  setup({"a"}, manifest, line + second);
  freeink::fakeNow = UINT32_MAX - 1000;
  SecureHttpClient::replies = {{200, "", 4000}, {200, "", 1000}};
  pluginevents::drain(nullptr, 4, {0, 5000});
  assert(SecureHttpClient::sends == 2 && Storage.files["/plugins/a/events.jsonl"] == second);

  // Cancellation before begin and just after client setup never starts transport.
  setup({}, manifest, "");
  String response;
  const pluginhttp::Headers headers;
  assert(pluginhttp::request(nullptr, "https://example.test/", "GET", "", headers, response, 1024, nullptr,
                             [] { return true; }) == -1);
  assert(pluginhttp::requestToFile(nullptr, "https://example.test/", "GET", "", headers, "/new.part", 1024,
                                   [] { return true; }) == -1);
  SecureHttpClient::beginDelayMs = 5000;
  assert(pluginhttp::request(nullptr, "https://example.test/", "GET", "", headers, response, 1024, nullptr,
                             [] { return freeink::fakeNow >= 5000; }) == -1);
  freeink::fakeNow = 0;
  assert(pluginhttp::requestToFile(nullptr, "https://example.test/", "GET", "", headers, "/new.part", 1024,
                                   [] { return freeink::fakeNow >= 5000; }) == -1);
  assert(SecureHttpClient::sends == 0 && !Storage.exists("/new.part"));
}
