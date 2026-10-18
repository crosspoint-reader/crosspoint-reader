// CMD:IDLE's decision (util/IdleGate.h): when may the device say it is idle? The loop samples it
// once per pass; these tests replay such passes. The case that matters most is the one a test
// session ran into: a press sent before IDLE, not yet handled while nothing looked busy, and IDLE
// answering before the page turned.

#include <gtest/gtest.h>

#include <cstring>

#include "util/IdleGate.h"

namespace {

using Verdict = IdleGate::Verdict;

// One loop pass every 10 ms, as on the device.
struct Device {
  IdleGate gate;
  uint32_t now = 1000;
  const char* busy = nullptr;
  bool handled = true;
  uint32_t passes = 0;

  Verdict pass() {
    now += 10;
    IdleGate::Sample sample;
    sample.nowMs = now;
    sample.busy = busy;
    sample.effectHandled = handled;
    sample.renderPasses = passes;
    return gate.step(sample);
  }

  // Passes until the wait ends; the verdict, with `now` left where it ended.
  Verdict runUntilDone(const uint32_t limitMs = 20000) {
    const uint32_t end = now + limitMs;
    while (now < end) {
      const Verdict v = pass();
      if (v != Verdict::Waiting) return v;
    }
    return Verdict::Waiting;
  }
};

}  // namespace

TEST(IdleGate, AQuietDeviceIsIdleAfterTheQuietWindow) {
  Device d;
  d.gate.begin(d.now, 10000);
  const uint32_t start = d.now;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
  EXPECT_GE(d.now - start, IdleGate::kQuietMs);
  EXPECT_LT(d.now - start, IdleGate::kQuietMs + 30);
}

TEST(IdleGate, BusyRestartsTheQuietWindow) {
  Device d;
  d.gate.begin(d.now, 10000);
  for (int i = 0; i < 20; ++i) EXPECT_EQ(d.pass(), Verdict::Waiting);  // 200 ms quiet
  d.busy = "render";
  EXPECT_EQ(d.pass(), Verdict::Waiting);
  d.busy = nullptr;
  const uint32_t quietFrom = d.now;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
  EXPECT_GE(d.now - quietFrom, IdleGate::kQuietMs);
}

// The race: the press is queued, nothing looks busy, and the input path has not handled it yet.
// However long that takes, IDLE must not answer before it is handled and its frame drawn.
TEST(IdleGate, AnUnhandledPressKeepsTheDeviceBusy) {
  Device d;
  d.gate.noteEffect();
  d.handled = false;
  d.gate.begin(d.now, 10000);
  for (int i = 0; i < 60; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting) << "600 ms, the press still queued";
  // Handled now, and its page turn queued in the same pass.
  d.handled = true;
  d.busy = "render";
  d.passes++;
  for (int i = 0; i < 30; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting);
  d.busy = nullptr;  // the frame is on the panel
  const uint32_t drawnAt = d.now;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
  EXPECT_GE(d.now - drawnAt, IdleGate::kQuietMs);
}

// Handled, but the screen acts on it a few passes later: wait for that render pass.
TEST(IdleGate, AHandledPressWaitsForTheFrameItCauses) {
  Device d;
  d.gate.noteEffect();
  d.gate.begin(d.now, 10000);
  for (int i = 0; i < 40; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting) << "no pass has started since";
  d.passes++;  // the page turn's render pass starts...
  d.busy = "render";
  for (int i = 0; i < 10; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting);
  d.busy = nullptr;  // ...and ends
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
}

// A press that changes nothing on screen (a key with no action here) must not hang IDLE.
TEST(IdleGate, APressThatDrawsNothingIsDoneAfterTheGrace) {
  Device d;
  d.gate.noteEffect();
  d.gate.begin(d.now, 10000);
  const uint32_t start = d.now;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
  EXPECT_GE(d.now - start, IdleGate::kNoRenderGraceMs);
  EXPECT_LT(d.now - start, IdleGate::kNoRenderGraceMs + IdleGate::kQuietMs + 50);
}

// A render pass that started before the press was handled belongs to something else.
TEST(IdleGate, APassBeforeThePressWasHandledDoesNotCount) {
  Device d;
  d.gate.noteEffect();
  d.handled = false;
  d.gate.begin(d.now, 10000);
  d.passes++;  // e.g. the reader's pre-render of the page before
  for (int i = 0; i < 10; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting);
  d.handled = true;  // handled with nothing under way yet
  for (int i = 0; i < 40; ++i) ASSERT_EQ(d.pass(), Verdict::Waiting) << "still waiting for its own frame";
  d.passes++;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
}

// An effect followed to its end before the host asks: IDLE then needs only the quiet window.
TEST(IdleGate, AnEffectSeenThroughBeforeTheWaitCostsNothingLater) {
  Device d;
  d.gate.noteEffect();
  d.busy = "render";
  EXPECT_TRUE(d.gate.active());
  d.pass();  // handled, its render under way
  d.busy = nullptr;
  d.pass();
  EXPECT_FALSE(d.gate.active()) << "nothing left to follow";
  d.gate.begin(d.now, 10000);
  const uint32_t start = d.now;
  EXPECT_EQ(d.runUntilDone(), Verdict::Idle);
  EXPECT_LT(d.now - start, IdleGate::kQuietMs + 30);
}

TEST(IdleGate, TimesOutNamingWhatKeptItBusy) {
  Device d;
  d.busy = "background";
  d.gate.begin(d.now, 500);
  EXPECT_EQ(d.runUntilDone(), Verdict::TimedOut);
  EXPECT_STREQ(d.gate.reason(), "background");
  EXPECT_FALSE(d.gate.waiting());
}

TEST(IdleGate, ATimeoutWithNothingBusyIsSettling) {
  Device d;
  d.gate.begin(d.now, 100);  // shorter than the quiet window
  EXPECT_EQ(d.runUntilDone(), Verdict::TimedOut);
  EXPECT_STREQ(d.gate.reason(), "settling");
}

TEST(IdleGate, AnUnhandledPressIsNamedInTheTimeout) {
  Device d;
  d.gate.noteEffect();
  d.handled = false;
  d.gate.begin(d.now, 300);
  EXPECT_EQ(d.runUntilDone(), Verdict::TimedOut);
  EXPECT_STREQ(d.gate.reason(), "input");
}
