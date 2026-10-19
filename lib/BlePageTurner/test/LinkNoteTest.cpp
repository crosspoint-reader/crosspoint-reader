// The note the book's status bar shows in place of its title: Connecting from entering the book
// until a remote links, Failed when the radio was refused or found nothing in 20 s, until the next
// page turn.

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

#include "Fakes.h"
#include "RadioPolicy.h"
#include "Runtime.h"

namespace {

using bleturner::LinkNote;
using bleturner::NoteInputs;
using bleturner::Scene;
using fake::host;
using fake::radio;

// --- The rule, pure -------------------------------------------------------------

NoteInputs in(const bool entered, const bool linked = false, const bool refused = false, const uint32_t runningMs = 0,
              const bool acknowledged = false) {
  NoteInputs i{};
  i.entered = entered;
  i.reading = true;
  i.enabled = true;
  i.linked = linked;
  i.refused = refused;
  i.runningMs = runningMs;
  i.acknowledged = acknowledged;
  return i;
}

TEST(LinkNoteRuleTest, Table) {
  struct Row {
    const char* name;
    LinkNote from;
    NoteInputs inputs;
    LinkNote to;
  };
  NoteInputs disabled = in(false);
  disabled.enabled = false;
  NoteInputs leaving = in(false);
  leaving.reading = false;
  NoteInputs idleEntry = in(true);
  idleEntry.idleStopped = true;
  const Row rows[] = {
      {"enter", LinkNote::None, in(true), LinkNote::Connecting},
      {"enter again after a failure", LinkNote::Failed, in(true), LinkNote::Connecting},
      {"enter with a remote still linked", LinkNote::None, in(true, true), LinkNote::None},
      {"enter with the radio off for idleness", LinkNote::None, idleEntry, LinkNote::None},
      {"waiting", LinkNote::Connecting, in(false), LinkNote::Connecting},
      {"link", LinkNote::Connecting, in(false, true), LinkNote::None},
      {"refused", LinkNote::Connecting, in(false, false, true), LinkNote::Failed},
      {"running just under 20 s", LinkNote::Connecting, in(false, false, false, 19999), LinkNote::Connecting},
      {"running 20 s without a link", LinkNote::Connecting, in(false, false, false, 20000), LinkNote::Failed},
      {"linked at 20 s", LinkNote::Connecting, in(false, true, false, 20000), LinkNote::None},
      {"failed stays", LinkNote::Failed, in(false), LinkNote::Failed},
      {"failed stays through a later link", LinkNote::Failed, in(false, true), LinkNote::Failed},
      {"acknowledge", LinkNote::Failed, in(false, false, false, 0, true), LinkNote::None},
      {"acknowledge while connecting", LinkNote::Connecting, in(false, false, false, 0, true), LinkNote::Connecting},
      {"none stays after a refusal", LinkNote::None, in(false, false, true, 30000), LinkNote::None},
      {"disabled", LinkNote::Connecting, disabled, LinkNote::None},
      {"disabled while failed", LinkNote::Failed, disabled, LinkNote::None},
      {"left the book", LinkNote::Failed, leaving, LinkNote::None},
  };
  for (const auto& row : rows) {
    EXPECT_EQ(bleturner::nextLinkNote(row.from, row.inputs), row.to) << row.name;
  }
}

// --- Through tick, against the fake radio ---------------------------------------

class LinkNoteTest : public ::testing::Test {
 public:
  void SetUp() override {
    fake::reset();
    config.enabled = 1;
    bleturner::begin(fake::hostFns(), config);
    radio().queueStarts = true;
  }
  void pass(const Scene& s) {
    bleturner::tick(s);
    if (radio().startQueued) radio().runStart();
  }
  static Scene home() {
    Scene s{};
    s.where = bleturner::Where::Elsewhere;
    return s;
  }
  bleturner::Config config;
};

TEST_F(LinkNoteTest, Journeys) {
  struct Row {
    const char* name;
    std::function<void(LinkNoteTest&)> steps;
    LinkNote expected;
  };
  const std::vector<Row> rows = {
      {"enter -> Connecting", [](LinkNoteTest& t) { t.pass(fake::reading()); }, LinkNote::Connecting},
      {"enter before the page is shown -> Connecting",
       [](LinkNoteTest& t) {
         Scene s = fake::reading();
         s.pageShown = false;
         t.pass(s);
       },
       LinkNote::Connecting},
      {"link -> None",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         radio().connected = true;
         t.pass(fake::reading());
       },
       LinkNote::None},
      {"refused for the heap -> Failed",
       [](LinkNoteTest& t) {
         host().heap = {65535, 32768};
         t.pass(fake::reading());
         t.pass(fake::reading());
       },
       LinkNote::Failed},
      {"rolled back after the stack came up -> Failed",
       [](LinkNoteTest& t) {
         host().heap = {83228, 61428};
         radio().changeHeapOnBegin = true;
         radio().heapAfterBegin = {28812, 26612};
         t.pass(fake::reading());
         t.pass(fake::reading());
       },
       LinkNote::Failed},
      {"running 19.999 s without a link -> Connecting",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         t.pass(fake::reading());  // the first pass that sees the radio up
         radio().now += bleturner::kLinkNoteFailMs - 1;
         t.pass(fake::reading());
       },
       LinkNote::Connecting},
      {"running 20 s without a link -> Failed",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         t.pass(fake::reading());
         radio().now += bleturner::kLinkNoteFailMs;
         t.pass(fake::reading());
       },
       LinkNote::Failed},
      {"acknowledge -> None",
       [](LinkNoteTest& t) {
         host().heap = {65535, 32768};
         t.pass(fake::reading());
         t.pass(fake::reading());
         ASSERT_EQ(bleturner::linkNote(), LinkNote::Failed);
         bleturner::acknowledgeLinkNote();
       },
       LinkNote::None},
      {"failed stays until acknowledged, a later link included",
       [](LinkNoteTest& t) {
         host().heap = {65535, 32768};
         t.pass(fake::reading());
         host().heap = {81920, 32768};
         for (int i = 0; i < 8; ++i) {
           radio().now += 1000;
           t.pass(fake::reading());
         }
         ASSERT_TRUE(radio().running) << "the retry started it";
         radio().connected = true;
         t.pass(fake::reading());
       },
       LinkNote::Failed},
      {"a later link after the acknowledgement stays None",
       [](LinkNoteTest& t) {
         host().heap = {65535, 32768};
         t.pass(fake::reading());
         t.pass(fake::reading());
         bleturner::acknowledgeLinkNote();
         radio().connected = true;
         t.pass(fake::reading());
         radio().connected = false;
         radio().now += 60000;
         t.pass(fake::reading());
       },
       LinkNote::None},
      {"acknowledge while connecting -> Connecting",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         bleturner::acknowledgeLinkNote();
       },
       LinkNote::Connecting},
      {"leave and enter again -> Connecting",
       [](LinkNoteTest& t) {
         host().heap = {65535, 32768};
         t.pass(fake::reading());
         t.pass(fake::reading());
         bleturner::acknowledgeLinkNote();
         t.pass(home());
         host().heap = {81920, 32768};
         t.pass(fake::reading(2));
       },
       LinkNote::Connecting},
      {"a new visit of the book -> Connecting",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         radio().connected = true;
         t.pass(fake::reading());
         radio().connected = false;
         t.pass(fake::reading(2));
       },
       LinkNote::Connecting},
      {"outside the book -> None",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         t.pass(home());
       },
       LinkNote::None},
      {"disabled -> None",
       [](LinkNoteTest& t) {
         t.config.enabled = 0;
         t.pass(fake::reading());
       },
       LinkNote::None},
      {"disabled while connecting -> None",
       [](LinkNoteTest& t) {
         t.pass(fake::reading());
         t.config.enabled = 0;
         t.pass(fake::reading());
       },
       LinkNote::None},
      {"remote still linked at entry -> None",
       [](LinkNoteTest& t) {
         radio().running = true;
         radio().connected = true;
         t.pass(fake::reading());
       },
       LinkNote::None},
      {"new book entry after idle stop -> Connecting",
       [](LinkNoteTest& t) {
         ASSERT_TRUE(bleturner::detail::stopForIdle());
         t.pass(fake::reading());
       },
       LinkNote::Connecting},
  };
  for (const auto& row : rows) {
    SetUp();
    row.steps(*this);
    EXPECT_EQ(bleturner::linkNote(), row.expected) << row.name;
  }
}

}  // namespace

// On the device the start task (priority 2) preempts the main loop (priority 1) on the
// single-core C3, so a quick heap refusal finishes inside startAsync(). queueStarts=false
// models that order.
namespace {
TEST(StartOrder, QuickRefusalInsideStartAsyncStillReportsFailedAndRetries) {
  using bleturner::LinkNote;
  fake::reset();
  bleturner::Config config;
  config.enabled = 1;
  bleturner::begin(fake::hostFns(), config);
  fake::radio().queueStarts = false;
  fake::host().heap = {65535, 32768};  // HeapLow: refused before the stack
  bleturner::tick(fake::reading());
  bleturner::tick(fake::reading());
  EXPECT_TRUE(bleturner::status().readerDeferred) << "refusal flag lost";
  EXPECT_EQ(bleturner::linkNote(), LinkNote::Failed);
  const unsigned createsBefore = fake::radio().creates;
  for (int i = 0; i < 12; ++i) {
    fake::radio().now += 1000;
    bleturner::tick(fake::reading());
  }
  EXPECT_GT(fake::radio().creates, createsBefore) << "no retry after a refusal";
}
}  // namespace
