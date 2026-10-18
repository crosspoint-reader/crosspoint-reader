"""Exercise production build pacing and reader admission with bounded host stubs."""

from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def method(path, signature):
    source = (ROOT / path).read_text()
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


section = method("lib/Epub/Epub/Section.cpp", "bool Section::buildSomeMore(")
reader = "src/activities/reader/EpubReaderActivity.cpp"
admission = "\n".join(method(reader, signature) for signature in (
    "bool EpubReaderActivity::backgroundInputActive()",
    "bool EpubReaderActivity::backgroundWorkAllowed()",
    "bool EpubReaderActivity::backgroundBuildWanted()",
    "bool EpubReaderActivity::skipLoopDelay()",
    "void EpubReaderActivity::runBackgroundWork()",
    "bool EpubReaderActivity::applyDeferredReposition()",
    "void EpubReaderActivity::clearDeferredReposition()",
))

harness = r"""
#include <atomic>
#include <cassert>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#define LOG_ERR(...) ((void)0)
#define LOG_DBG(...) ((void)0)
struct ReaderRenderSpec {};
struct ChapterHtmlSlimParser {
  enum class ParseStatus { More, Done, Error };
  struct Step { ParseStatus status; int pages; };
  std::vector<Step> steps;
  unsigned calls = 0;
  int* pages;
  ParseStatus parseStep() {
    const auto step = steps.at(calls++);
    *pages += step.pages;
    return step.status;
  }
  unsigned parseBytesConsumed() const { return calls * 1024; }
};
struct Section {
  struct Build { std::unique_ptr<ChapterHtmlSlimParser> parser; unsigned bytesConsumed = 0; };
  std::unique_ptr<Build> build_;
  int builtPageCount_ = 0;
  int pageCount = 100, currentPage = 0;
  bool building = true, partial = false, finalized = false, failed = false;
  bool checkpointOk = true;
  int checkpoints = 0, starts = 0;
  std::optional<int> offsetPage;
  std::optional<int> getPageForVisibleTextOffset(uint32_t) const { return offsetPage; }
  bool isBuilding() const { return building; }
  bool isBuildComplete() const { return finalized; }
  bool isPartial() const { return partial; }
  bool startBuild(const ReaderRenderSpec&) { ++starts; building = true; return true; }
  void suspendBuild() { ++checkpoints; building = false; partial = checkpointOk; if (!partial) pageCount = 0; }
  void abandonBuild() { failed = true; build_.reset(); }
  bool finalizeBuild() { finalized = true; build_.reset(); return true; }
  bool buildSomeMore(int maxPages, int maxParseSteps = 0);
  void start(std::vector<ChapterHtmlSlimParser::Step> steps) {
    build_ = std::make_unique<Build>();
    build_->parser = std::make_unique<ChapterHtmlSlimParser>();
    build_->parser->steps = std::move(steps);
    build_->parser->pages = &builtPageCount_;
    builtPageCount_ = 0; finalized = failed = false;
  }
};
uint32_t now = 1000;
uint32_t millis() { return now; }
enum class HomeButtonAction { Ignore, ReaderMenu };
struct GPIO {
  bool raw = false, pressed = false, released = false, touch = false, heldTouch = false;
  uint8_t heldButtons = 0, capacitive = 0;
  bool rawInputActive() const { return raw; }
  bool wasAnyPressed() const { return pressed; }
  bool wasAnyReleased() const { return released; }
  bool wasTouchActivity() const { return touch; }
  bool isTouchHeldAt(float&, float&) const { return heldTouch; }
  bool isPressed(uint8_t key) const { return heldButtons & (1 << key); }
  bool isCapacitivePagePressed(uint8_t key) const { return capacitive & (1 << key); }
} gpio;
struct HalGPIO { enum { BTN_BACK = 0, BTN_POWER = 6 }; };
struct Input {
  bool remote = false;
  HomeButtonAction home = HomeButtonAction::Ignore;
  bool hasRemoteClick() const { return remote; }
  HomeButtonAction homeButtonAction() const { return home; }
};
struct Manager { bool pending = false; bool hasPendingUiWork() const { return pending; } } activityManager;
struct RenderLock {
  enum class Mode { Try };
  explicit RenderLock(Mode) {}
  bool ownsLock() const { return true; }
};
struct HalPowerManager { struct Lock { static inline int count = 0; Lock() { ++count; } }; };
struct Settings { ReaderRenderSpec readerRenderSpec(int, int) const { return {}; } } SETTINGS;
struct Renderer { unsigned frameBufferLoanCount() const { return 0; } };
struct SectionHandle {
  Section* value = nullptr;
  SectionHandle& operator=(Section* p) { value = p; return *this; }
  explicit operator bool() const { return value != nullptr; }
  Section* operator->() const { return value; }
  void reset() { value = nullptr; }
};
namespace bleturner { bool held = false; bool holdsHeap() { return held; } }
struct EpubReaderActivity {
  enum class Overlay { None, Toolbar };
  Overlay overlay = Overlay::None;
  SectionHandle section;
  Renderer renderer;
  Input mappedInput;
  int pendingManualTurn = 0;
  bool buildHeapPaused = false;
  bool gate = true, pageBufferStale = false, partialRebuildStartFailed = false;
  int currentSpineIndex = 5, idleBuildFailedSpine = -1, updates = 0;
  int buildViewportWidth = 528, buildViewportHeight = 792;
  std::optional<int> pendingPageJump;
  int cachedSpineIndex = 5, cachedChapterTotalPageCount = 0;
  std::optional<uint32_t> cachedVisibleTextOffset;
  static constexpr int PARTIAL_REBUILD_START_MARGIN = 15;
  static constexpr int BACKGROUND_BUILD_PAGES_PER_TICK = 2;
  bool buildTickHeapGate() { return gate; }
  void requestUpdate() { ++updates; }
  bool applyDeferredReposition();
  void clearDeferredReposition();
  std::atomic<bool> radioReleasedForBuild{false};
  uint32_t lastRenderCompleteMs = 100, lastInputMs = 100;
  static constexpr int BUILD_WINDOW_AHEAD = 5;
  static constexpr unsigned IDLE_WORK_DEBOUNCE_MS = 400;
  bool backgroundInputActive() const;
  bool backgroundWorkAllowed() const;
  bool backgroundBuildWanted() const;
  bool skipLoopDelay();
  void runBackgroundWork();
};
__PRODUCTION__
int main() {
  using S = ChapterHtmlSlimParser::ParseStatus;
  Section section;
  // A long no-page prefix must yield and retain its parse watermark.
  section.start({{S::More, 0}, {S::More, 0}, {S::More, 3}, {S::Done, 1}});
  for (unsigned i = 1; i <= 3; ++i) {
    assert(section.buildSomeMore(2, 1));
    assert(section.build_->parser->calls == i);
    assert(section.build_->bytesConsumed == i * 1024);
    assert(!section.finalized);
  }
  assert(section.buildSomeMore(2, 1) && section.finalized && !section.build_);
  assert(section.builtPageCount_ == 4);
  // Unbounded callers still finish, and partial watermarks do not pace new builds.
  section.start({{S::More, 0}, {S::More, 1}, {S::More, 1}, {S::Done, 1}});
  assert(section.buildSomeMore(2));
  assert(section.build_->parser->calls == 3 && section.builtPageCount_ == 2);
  assert(section.buildSomeMore(0) && section.finalized);
  section.start({{S::More, 0}, {S::Error, 0}});
  assert(section.buildSomeMore(2, 1));
  assert(!section.buildSomeMore(2, 1) && section.failed && !section.build_);
  assert(!section.buildSomeMore(2, 1));

  EpubReaderActivity reader;
  reader.section = &section;
  assert(reader.skipLoopDelay());
  reader.idleBuildFailedSpine = reader.currentSpineIndex;
  assert(!reader.backgroundBuildWanted());
  section.pageCount = 4;
  assert(reader.backgroundBuildWanted()); // mandatory work keeps its existing failure handling
  section.pageCount = 100;
  reader.idleBuildFailedSpine = -1;
  reader.lastInputMs = now;
  assert(!reader.backgroundBuildWanted());
  section.pageCount = 4;
  assert(reader.backgroundBuildWanted()); // mandatory look-ahead bypasses debounce
  section.pageCount = 100;
  section.partial = true;
  assert(reader.backgroundBuildWanted());
  section.partial = false;
  reader.radioReleasedForBuild = true;
  assert(reader.backgroundBuildWanted());
  bleturner::held = true;
  assert(!reader.backgroundBuildWanted());
  bleturner::held = false;
  reader.buildHeapPaused = true;
  assert(!reader.skipLoopDelay());
  reader.buildHeapPaused = false;
  activityManager.pending = true;
  assert(!reader.skipLoopDelay());
  activityManager.pending = false;
  reader.pendingManualTurn = 1;
  assert(!reader.skipLoopDelay());
  reader.pendingManualTurn = 0;
  reader.overlay = EpubReaderActivity::Overlay::Toolbar;
  assert(!reader.skipLoopDelay());
  reader.overlay = EpubReaderActivity::Overlay::None;
  for (bool* flag : {&gpio.raw, &gpio.pressed, &gpio.released, &gpio.touch, &gpio.heldTouch,
                     &reader.mappedInput.remote}) {
    *flag = true; assert(!reader.skipLoopDelay()); *flag = false;
  }
  for (int key = 0; key <= 6; ++key) {
    gpio.heldButtons = 1 << key; assert(!reader.skipLoopDelay()); gpio.heldButtons = 0;
    gpio.capacitive = 1 << key; assert(!reader.skipLoopDelay()); gpio.capacitive = 0;
  }
  reader.mappedInput.home = HomeButtonAction::ReaderMenu;
  assert(!reader.skipLoopDelay());
  reader.mappedInput.home = HomeButtonAction::Ignore;
  reader.radioReleasedForBuild = false;
  reader.lastInputMs = reader.lastRenderCompleteMs = UINT32_MAX - 500;
  now = 100;
  assert(reader.skipLoopDelay()); // millis wraparound
  section.building = false;
  assert(!reader.skipLoopDelay());

  // Heap pressure checkpoints only a fresh build with enough readable pages ahead.
  section.building = true;
  section.currentPage = 1;
  section.pageCount = 514;
  reader.gate = false;
  reader.runBackgroundWork();
  assert(section.checkpoints == 1 && section.isPartial() && !section.isBuilding());
  reader.runBackgroundWork();
  assert(section.starts == 0 && section.checkpoints == 1);
  section.currentPage = 499;
  reader.runBackgroundWork();
  assert(section.starts == 1 && section.isBuilding());
  const int powerLocks = HalPowerManager::Lock::count;
  reader.runBackgroundWork(); // partial rebuild is mandatory; no checkpoint/restart loop
  assert(section.checkpoints == 1 && HalPowerManager::Lock::count == powerLocks);
  section.partial = false;
  section.currentPage = 1;
  reader.radioReleasedForBuild = true;
  reader.runBackgroundWork();
  assert(section.checkpoints == 1 && HalPowerManager::Lock::count == powerLocks);
  reader.radioReleasedForBuild = false;
  section.pageCount = 16; // exactly the restart margin: cannot checkpoint
  reader.runBackgroundWork();
  assert(section.checkpoints == 1 && HalPowerManager::Lock::count == powerLocks);

  // A failed checkpoint restores the visible page and disables optional retries.
  section.pageCount = 514;
  section.checkpointOk = false;
  reader.runBackgroundWork();
  assert(!reader.section && reader.pendingPageJump == 1 && reader.updates == 1);
  assert(reader.idleBuildFailedSpine == reader.currentSpineIndex);
  reader.section = &section;
  section.building = true;
  section.pageCount = 9;
  assert(!reader.backgroundBuildWanted());

  // A parser failure has the same recovery without changing foreground callers.
  section.start({{S::Error, 0}});
  reader.gate = true;
  reader.idleBuildFailedSpine = -1;
  reader.runBackgroundWork();
  assert(!reader.section && reader.pendingPageJump == 1 && reader.updates == 2);
  assert(reader.idleBuildFailedSpine == reader.currentSpineIndex);

  // A refresh of a pressure-checkpointed chapter must not scale legacy progress
  // against the partial watermark or discard its deferred final mapping.
  reader.section = &section;
  section.building = false;
  section.partial = true;
  section.currentPage = 100;
  section.pageCount = 514;
  reader.cachedChapterTotalPageCount = 2000;
  assert(!reader.applyDeferredReposition());
  assert(section.currentPage == 100 && reader.cachedChapterTotalPageCount == 2000);
  section.partial = false;
  section.pageCount = 1500;
  assert(reader.applyDeferredReposition());
  assert(section.currentPage == 75 && reader.cachedChapterTotalPageCount == 0);

  // Offset-based progress also waits for completion, then takes precedence.
  section.partial = true;
  section.currentPage = 100;
  section.pageCount = 514;
  section.offsetPage = 80;
  reader.cachedVisibleTextOffset = 12345;
  reader.cachedChapterTotalPageCount = 2000;
  assert(!reader.applyDeferredReposition());
  assert(section.currentPage == 100 && reader.cachedVisibleTextOffset == 12345);
  section.partial = false;
  section.building = true;
  assert(!reader.applyDeferredReposition());
  section.building = false;
  section.pageCount = 1500;
  assert(reader.applyDeferredReposition());
  assert(section.currentPage == 80 && !reader.cachedVisibleTextOffset.has_value());
  assert(reader.cachedChapterTotalPageCount == 0);
}
"""

with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(harness.replace("__PRODUCTION__", section + "\n" + admission))
    subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=undefined,address",
                    str(path / "test.cpp"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("Background pagination pacing and admission passed")
