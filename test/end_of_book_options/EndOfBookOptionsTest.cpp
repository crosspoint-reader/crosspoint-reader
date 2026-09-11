#include <gtest/gtest.h>

#include <algorithm>
#include <limits>

#include "ReaderActivity.h"

// The host links only the shared menu path. EPUB parsing, SD I/O and panel
// rendering are outside this fixture.
void ReaderActivity::onEnter() {}
void ReaderActivity::onExit() {}
void ReaderActivity::loop() {}
void ReaderActivity::render(RenderLock&&) {}
void ReaderActivity::applyInitialOrientation() {}
bool ReaderActivity::handleForcedRefresh() { return false; }

namespace {

class MenuReader : public ReaderActivity {
 public:
  bool atEnd = true;
  int lastPages = 0;
  MenuReader(GfxRenderer& renderer, MappedInputManager& input)
      : ReaderActivity("TestReader", renderer, input, "/books/current.epub", false) {
    endOfBookOptions = std::make_unique<EndOfBookOptions>(renderer);
    endOfBookOptions->loadOnce(bookPath);
    endOfBookOptionsReady.store(true);
  }
  using ReaderActivity::handleEndOfBookMenu;
  void renderMenu() { endOfBookOptions->render(renderer, mappedInput); }
  void unpublish() { endOfBookOptionsReady.store(false); }

 private:
  bool loadBook() override { return true; }
  std::string getBookTitle() const override { return ""; }
  bool pageTurn(bool) override { return false; }
  bool isAtEndOfBook() const override { return atEnd; }
  void onReturnFromEndOfBook() override { ++lastPages; }
  void renderBook() override {}
};

class EndOfBookOptionsTest : public ::testing::Test {
 protected:
  GfxRenderer renderer;
  MappedInputManager input;
  std::unique_ptr<MenuReader> reader;

  void SetUp() override {
    marquee_test::now = 100;
    marquee_test::screen = {};
    marquee_test::books.clear();
    activityManager = {};
    SETTINGS = {};
    gpio = {};
  }
  void open(const std::vector<std::string>& titles) {
    marquee_test::books.clear();
    for (const auto& title : titles) marquee_test::books.push_back(title + ".epub");
    reader = std::make_unique<MenuReader>(renderer, input);
    reader->renderMenu();
  }
  std::string label() const { return marquee_test::screen.labels[marquee_test::screen.selected]; }
  bool tick(uint32_t now) {
    marquee_test::now = now;
    const int before = reader->updates;
    EXPECT_FALSE(reader->handleEndOfBookMenu());
    if (reader->updates == before) return false;
    EXPECT_EQ(reader->updates, before + 1);
    reader->renderMenu();
    return true;
  }
  void next() {
    input.pressed = static_cast<int>(MappedInputManager::Button::NavNext);
    EXPECT_TRUE(reader->handleEndOfBookMenu());
    input = {};
    reader->renderMenu();
  }
};

TEST_F(EndOfBookOptionsTest, IdleReaderRevealsEntireNumberedTitleAndRestartsAfterPauses) {
  // Synthetic fixture: no assumption about a filename on a user's device.
  const std::string title = "Emperor\u2019s Domination \u2014 Volume 012345";
  marquee_test::screen.content.width = 264;
  open({title});
  const std::string first = label();
  ASSERT_NE(first, title);
  ASSERT_TRUE(title.starts_with(first));
  ASSERT_EQ(first, "Emperor\u2019s Domination \u2014 Volume 0");
  EXPECT_FALSE(tick(1599));
  ASSERT_TRUE(tick(1600));
  EXPECT_NE(label(), first);

  uint32_t now = 1600;
  std::string windows = first + label();
  for (int i = 0; !label().ends_with("012345") && i < 100; ++i) {
    now += 300;
    ASSERT_TRUE(tick(now));
    const auto visible = label();
    EXPECT_NE(title.find(visible), std::string::npos);
    EXPECT_LE(marquee_test::screen.target().measureText(0, visible.c_str(), {}).width, 248);
    windows += visible;
  }
  const std::string last = label();
  ASSERT_TRUE(last.ends_with("Volume 012345"));
  EXPECT_NE(windows.find("\u2019"), std::string::npos);
  EXPECT_NE(windows.find("\u2014"), std::string::npos);
  EXPECT_FALSE(tick(now + 1499));
  EXPECT_EQ(label(), last);
  ASSERT_TRUE(tick(now + 1500));
  EXPECT_EQ(label(), first);
  EXPECT_FALSE(tick(now + 2999));
  EXPECT_TRUE(tick(now + 3000));
}

TEST_F(EndOfBookOptionsTest, IncidentalRedrawDoesNotRestartInitialOrStepPause) {
  open({"A sufficiently long title with a distinct ending 123"});
  const auto first = label();
  marquee_test::now = 700;
  reader->renderMenu();
  EXPECT_EQ(label(), first);
  ASSERT_TRUE(tick(1600));
  const auto second = label();
  marquee_test::now = 1700;
  reader->renderMenu();
  EXPECT_EQ(label(), second);
  EXPECT_FALSE(tick(1899));
  EXPECT_TRUE(tick(1900));
}

TEST_F(EndOfBookOptionsTest, ShortTitleAndHomeStopWhileNewLongSelectionGetsInitialPause) {
  open({"Long title with enough text to overflow the row 1", "Short", "Another long title ending in 123456"});
  ASSERT_TRUE(tick(1600));
  next();
  EXPECT_EQ(label(), "Short");
  EXPECT_FALSE(tick(3000));
  next();
  const auto initial = label();
  EXPECT_FALSE(tick(4499));
  EXPECT_TRUE(tick(4500));
  EXPECT_NE(label(), initial);
  next();
  EXPECT_EQ(label(), "Home");
  EXPECT_FALSE(tick(10000));
}

TEST_F(EndOfBookOptionsTest, SingleOversizedCodepointStaysIdleAndAnotherSelectionCanScroll) {
  for (const std::string glyph : {"W", "\u754c"}) {
    SCOPED_TRACE(glyph);
    marquee_test::now = 100;
    // Four pixels remain after padding; the mock measures each codepoint at eight.
    marquee_test::screen.content.width = 20;
    open({glyph, "Long title"});
    EXPECT_EQ(label(), glyph);
    for (const uint32_t now : {1600U, 3100U, 6100U}) {
      EXPECT_FALSE(tick(now));
      EXPECT_EQ(label(), glyph);
    }
    marquee_test::now = 7000;
    reader->renderMenu();
    EXPECT_FALSE(tick(8500));
    EXPECT_EQ(label(), glyph);

    next();
    EXPECT_EQ(label(), "L");
    EXPECT_FALSE(tick(9999));
    ASSERT_TRUE(tick(10000));
    EXPECT_EQ(label(), "o");
    next();  // Home
    next();  // Return to the oversized glyph.
    EXPECT_EQ(label(), glyph);
    EXPECT_FALSE(tick(11500));
  }
}

TEST_F(EndOfBookOptionsTest, MalformedUtf8CannotSkipAnUnboundedRunOfContinuationBytes) {
  marquee_test::screen.content.width = 20;
  std::string title = "A";
  title.append(8, static_cast<char>(0x80));
  title += "long title";
  open({title});
  std::string initial = "A";
  initial.append(3, static_cast<char>(0x80));
  EXPECT_EQ(label(), initial);

  ASSERT_TRUE(tick(1600));
  EXPECT_EQ(label(), std::string(5, static_cast<char>(0x80)));
  ASSERT_TRUE(tick(1900));
  EXPECT_EQ(label(), std::string(1, static_cast<char>(0x80)));
  ASSERT_TRUE(tick(2200));
  EXPECT_EQ(label(), "l");
}

TEST_F(EndOfBookOptionsTest, DeadlineDoesNotConsumeLongBackOrReplaceNavigationActions) {
  open({"A sufficiently long title with an ending 123"});
  marquee_test::now = 1600;
  input.released = static_cast<int>(MappedInputManager::Button::Back);
  input.held = 1500;
  // False lets the reader's normal long-Back handler process this same event.
  EXPECT_FALSE(reader->handleEndOfBookMenu());
  EXPECT_EQ(reader->updates, 1);
  input.held = 100;
  EXPECT_TRUE(reader->handleEndOfBookMenu());
  EXPECT_EQ(reader->lastPages, 1);
  EXPECT_EQ(reader->updates, 2);
  input = {};
  input.released = static_cast<int>(MappedInputManager::Button::Confirm);
  EXPECT_TRUE(reader->handleEndOfBookMenu());
  EXPECT_EQ(activityManager.openedPath, "/books/A sufficiently long title with an ending 123.epub");
  EXPECT_EQ(reader->updates, 2);
}

TEST_F(EndOfBookOptionsTest, TouchOpensBookAndHomeWithoutWaitingForAnimation) {
  open({"Long title that needs to scroll before its ending 123"});
  input.tappedRow = 0;
  EXPECT_TRUE(reader->handleEndOfBookMenu());
  EXPECT_EQ(activityManager.openedPath, "/books/Long title that needs to scroll before its ending 123.epub");
  input.tappedRow = 1;
  EXPECT_TRUE(reader->handleEndOfBookMenu());
  EXPECT_EQ(activityManager.homes, 1);
}

TEST_F(EndOfBookOptionsTest, InactiveUnpublishedAndSuppressedMenusDoNotSchedule) {
  open({"Long title that needs to scroll before its ending 123"});
  marquee_test::now = 1600;
  EXPECT_FALSE(reader->handleEndOfBookMenu(true));
  reader->atEnd = false;
  EXPECT_FALSE(reader->handleEndOfBookMenu());
  reader->atEnd = true;
  reader->unpublish();
  EXPECT_FALSE(reader->handleEndOfBookMenu());
  EXPECT_EQ(reader->updates, 0);
  open({});
  EXPECT_FALSE(reader->handleEndOfBookMenu());
  EXPECT_EQ(reader->updates, 0);
}

TEST_F(EndOfBookOptionsTest, BuffersAt512BytesNeverSplitMultibyteCodepoints) {
  marquee_test::screen.content.width = 10000;
  std::string title;
  for (int i = 0; i < 1300; ++i) title += "\u754c";
  open({title});
  EXPECT_EQ(label().size(), 510U);
  EXPECT_EQ(label(), title.substr(0, 510));
  ASSERT_TRUE(tick(1600));
  EXPECT_EQ(label().size(), 510U);
  EXPECT_EQ(label(), title.substr(3, 510));
  std::string ascii(1400, 'a');
  open({ascii});
  EXPECT_EQ(label().size(), 512U);
}

TEST_F(EndOfBookOptionsTest, InitialDeadlineCanCrossOrLandExactlyOnClockRollover) {
  for (const uint32_t start : {UINT32_MAX - 500U, UINT32_MAX - 1499U}) {
    marquee_test::now = start;
    open({"Long title that needs to scroll before its ending 123"});
    const uint32_t expected = start + 1500U == 0 ? 1U : start + 1500U;
    EXPECT_FALSE(tick(expected - 1));
    EXPECT_TRUE(tick(expected));
  }
}

TEST_F(EndOfBookOptionsTest, StepAndEndPauseCanLandExactlyOnClockRollover) {
  const uint32_t firstShift = UINT32_MAX - 299U;
  marquee_test::now = firstShift - 1500U;
  open({"Long title that needs many scrolling steps before its ending 123"});
  ASSERT_TRUE(tick(firstShift));
  EXPECT_FALSE(tick(0));
  EXPECT_TRUE(tick(1));

  marquee_test::now = UINT32_MAX - 2999U;
  open({std::string(19, 'x')});  // 18 characters fit; one shift reaches the end.
  ASSERT_TRUE(tick(UINT32_MAX - 1499U));
  const auto tail = label();
  EXPECT_FALSE(tick(0));
  EXPECT_EQ(label(), tail);
  EXPECT_TRUE(tick(1));
  EXPECT_EQ(label().size(), 18U);
}

TEST_F(EndOfBookOptionsTest, HiddenSelectionDoesNotAnimate) {
  marquee_test::screen.content.height = 20;
  open({"Short", "Long title that does not fit in the first row 123"});
  next();
  EXPECT_FALSE(tick(2000));
}

}  // namespace
