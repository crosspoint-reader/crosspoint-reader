"""Exercise production manager transitions with allocation and rendering fault models."""

from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = Path(sys.argv[1]).read_text() if len(sys.argv) > 1 else (
    ROOT / "src/activities/settings/HyphenationManagerActivity.cpp"
).read_text()


def method(signature, following):
    return signature + source.split(signature, 1)[1].split(following, 1)[0]


methods = "\n".join([
    method("void HyphenationManagerActivity::onEnter()", "void HyphenationManagerActivity::onExit()"),
    method("void HyphenationManagerActivity::onExit()", "bool HyphenationManagerActivity::loadManifest()"),
    method("bool HyphenationManagerActivity::downloadPack(Pack& pack)",
           "void HyphenationManagerActivity::confirmRemoval"),
])

harness = r"""
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#define LOG_ERR(...) ((void)0)
#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition); \
  std::exit(1); } } while (false)
static int locks = 0, releases = 0, requests = 0, finishes = 0;
static int starts = 0, readerRestarts = 0, settingsRestarts = 0;
static bool failRows = false, failWifi = false, installed = false;
static size_t freeHeap = 100000, maxBlock = 100000;
namespace fui { struct ListItem {}; }
struct ActivityResult { bool isCancelled = false; };
struct WifiSelectionActivity {
  template <typename R, typename I> WifiSelectionActivity(R&, I&) {}
};
template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if constexpr (std::is_array_v<T>) { if (failRows) return nullptr; }
  else { if (failWifi) return nullptr; }
  return std::make_unique<T>(std::forward<Args>(args)...);
}
struct Cache {
  void releaseSdFontCaches() { CHECK(locks > 0); ++releases; }
};
struct Renderer {
  Cache cache;
  Cache* getFontCacheManager() { return &cache; }
};
struct UiListActivity { void onEnter() {} void onExit() {} };
struct RenderLock {
  template <typename A> explicit RenderLock(A&) { ++locks; }
  ~RenderLock() { --locks; }
};
constexpr int WIFI_OFF = 0, WIFI_MODE_NULL = 0, WIFI_STA = 1;
struct Wifi {
  int current = WIFI_OFF;
  int modeCalls = 0, disconnects = 0;
  void mode(int value) { current = value; ++modeCalls; }
  int getMode() { return current; }
  void disconnect(bool) { ++disconnects; }
} WiFi;
void delay(int) {}
void silentRestartToReader() { ++readerRestarts; }
void silentRestartToSettings() { ++settingsRestarts; }
struct Esp {
  size_t getFreeHeap() { return freeHeap; }
  size_t getMaxAllocHeap() { return maxBlock; }
} ESP;
struct HalFile { size_t size() { return 100; } void close() {} };
struct Store {
  void remove(const char*) {}
  bool openFileForRead(const char*, const char*, HalFile&) { return true; }
} Storage;
struct HttpDownloader {
  static constexpr int MIN_TLS_FREE_HEAP = 50000, MIN_TLS_MAX_ALLOC = 20000, OK = 0;
  static int downloadToFile(const std::string&, const char*, void*, void*, const char*, const char*, int, bool) {
    CHECK(locks == 0); return -1;
  }
};
namespace HyphenationPackStore {
enum class InstallResult { OK };
bool isInstalled(const char*) { return installed; }
InstallResult installFromSd(const char*) { return InstallResult::OK; }
}
constexpr char PACK_TMP[] = "pack";
struct HyphenationManagerActivity : UiListActivity {
  struct Pack { char code[3] = {'d', 'e', '\0'}; unsigned size = 100, fileCrc = 0; };
  static constexpr int MAX_PACKS = 48;
  Renderer renderer;
  int mappedInput = 0;
  std::unique_ptr<fui::ListItem[]> rows_;
  std::string baseUrl_ = "https://example.com/";
  char wantedCode_[3] = {}, failedCode_[3] = {};
  bool wifiStarted_ = false, loading_ = false;
  int offlineLoads = 0;
  void loadOfflineList() { ++offlineLoads; }
  void scanSdRoot() {}
  void finish() { ++finishes; }
  void requestUpdate() { ++requests; }
  void requestUpdateAndWait() { CHECK(locks == 0); ++requests; }
  void onWifiReady(bool) {}
  template <typename Callback>
  void startActivityForResult(std::unique_ptr<WifiSelectionActivity>, Callback) { ++starts; }
  bool fileCrcMatches(const char*, unsigned) { return true; }
  void onEnter(); void onExit(); bool downloadPack(Pack&);
};
""" + methods + r"""
static void enter(HyphenationManagerActivity& manager) {
  // ActivityManager unlocks before entry; cache release must lock locally.
  manager.onEnter();
}
int main() {
  HyphenationManagerActivity allocationFailure;
  failWifi = true; enter(allocationFailure);
  CHECK(allocationFailure.offlineLoads == 1 && allocationFailure.rows_);
  CHECK(WiFi.modeCalls == 0 && !allocationFailure.wifiStarted_ && starts == 0);
  CHECK(requests == 1);
  allocationFailure.onExit();
  CHECK(readerRestarts == 0 && settingsRestarts == 0);

  failWifi = false;
  HyphenationManagerActivity offlineInstall;
  offlineInstall.wantedCode_[0] = 'd'; offlineInstall.wantedCode_[1] = 'e';
  installed = true; enter(offlineInstall);
  CHECK(finishes == 1 && WiFi.modeCalls == 0);
  offlineInstall.onExit(); CHECK(readerRestarts == 1);

  installed = false;
  HyphenationManagerActivity online;
  enter(online); CHECK(WiFi.current == WIFI_STA && online.wifiStarted_ && starts == 1);
  online.onExit(); CHECK(settingsRestarts == 1 && WiFi.disconnects == 1);

  HyphenationManagerActivity cancelled;
  cancelled.wantedCode_[0] = 'd'; cancelled.wantedCode_[1] = 'e';
  enter(cancelled); cancelled.onExit();
  CHECK(WiFi.current == WIFI_OFF && readerRestarts == 1);

  HyphenationManagerActivity download;
  HyphenationManagerActivity::Pack pack;
  const int before = releases;
  CHECK(!download.downloadPack(pack));
  CHECK(releases == before + 1 && !download.loading_ && locks == 0);
  freeHeap = 100;
  CHECK(!download.downloadPack(pack));
  CHECK(!download.loading_ && locks == 0);

  failRows = true;
  HyphenationManagerActivity noRows; enter(noRows);
  CHECK(!noRows.rows_ && noRows.offlineLoads == 0 && finishes == 2);
  std::puts("Manager lifecycle checks passed (7 fault/transition scenarios)");
}
"""

with tempfile.TemporaryDirectory() as directory:
    cpp = Path(directory) / "lifecycle.cpp"
    binary = Path(directory) / "lifecycle"
    cpp.write_text(harness)
    subprocess.run([
        "c++", "-std=c++20", "-DNDEBUG", "-Wall", "-Wextra", "-Werror",
        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp), "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary)], check=True)
