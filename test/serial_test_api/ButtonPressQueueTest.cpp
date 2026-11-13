// CMD:BTN's presses (util/ButtonPressQueue.h) played against a model of the SDK's InputManager
// debounce: a raw change commits once it has been stable for more than 5 ms at an update. Each
// pass of the model loop runs one input update with the buttons held so far, then one step().

#include <gtest/gtest.h>

#include "util/ButtonPressQueue.h"
#include "util/IdleGate.h"

namespace {

constexpr uint8_t BACK = 0;
constexpr uint8_t CONFIRM = 1;

struct InputModel {
  uint8_t lastState = 0;
  uint8_t currentState = 0;
  uint32_t lastDebounceTime = 0;
  uint8_t pressedEvents = 0;
  uint8_t releasedEvents = 0;
  uint32_t pressStart = 0;
  uint32_t pressFinish = 0;
  bool ignoresHook = false;  // a board whose input path never reads the hook
  uint8_t realKeys = 0;      // keys a person holds down, read along with the hook

  void update(const uint32_t now, uint8_t raw) {
    if (ignoresHook) raw = 0;
    raw |= realKeys;
    pressedEvents = 0;
    releasedEvents = 0;
    if (raw != lastState) {
      lastDebounceTime = now;
      lastState = raw;
    }
    if (now - lastDebounceTime > 5 && raw != currentState) {
      pressedEvents = raw & ~currentState;
      releasedEvents = currentState & ~raw;
      if (pressedEvents && currentState == 0) pressStart = now;
      if (releasedEvents && raw == 0) pressFinish = now;
      currentState = raw;
    }
  }
};

struct Loop {
  ButtonPressQueue queue;
  InputModel input;
  uint32_t now = 1000;
  uint32_t passMs = 10;
  uint8_t held = 0;
  int presses[8] = {};
  int releases[8] = {};
  uint32_t lastHeldMs = 0;

  void pass() {
    now += passMs;
    input.update(now, held);
    for (int b = 0; b < 8; ++b) {
      if (input.pressedEvents & (1u << b)) presses[b]++;
      if (input.releasedEvents & (1u << b)) {
        releases[b]++;
        lastHeldMs = input.pressFinish - input.pressStart;
      }
    }
    held = queue.step(now, input.currentState);
  }

  // Passes until the queue is done; false if it never was.
  bool runUntilIdle(const int maxPasses = 2000) {
    for (int i = 0; i < maxPasses; ++i) {
      pass();
      if (queue.idle()) return true;
    }
    return false;
  }
};

}  // namespace

TEST(ButtonPressQueue, AShortPressIsOneCommittedPressAndRelease) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  EXPECT_FALSE(loop.queue.idle());
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.releases[CONFIRM], 1);
  EXPECT_GE(loop.lastHeldMs, ButtonPressQueue::SHORT_HOLD_MS);
  EXPECT_LT(loop.lastHeldMs, 200u) << "well below the shortest long press (350 ms)";
  EXPECT_EQ(loop.held, 0);
}

TEST(ButtonPressQueue, ALongPressIsHeldPastTheOneSecondThreshold) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(BACK, ButtonPressQueue::LONG_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[BACK], 1);
  EXPECT_GT(loop.lastHeldMs, 1000u);
  EXPECT_LT(loop.lastHeldMs, 1500u) << "short of the 1.5 s holds";
}

// Idle passes run up to 50 ms apart; a press must still be committed, never lost between samples.
TEST(ButtonPressQueue, SlowPassesStillCommitThePress) {
  Loop loop;
  loop.passMs = 50;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.releases[CONFIRM], 1);
}

// Fast passes: the commit needs more than 5 ms, whatever the number of samples.
TEST(ButtonPressQueue, FastPassesWaitForTheDebounce) {
  Loop loop;
  loop.passMs = 1;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_GE(loop.lastHeldMs, ButtonPressQueue::SHORT_HOLD_MS);
}

// A double press: two presses of one button, each released and committed before the next.
TEST(ButtonPressQueue, TwoPressesOfOneButtonStayTwoPresses) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  const uint32_t start = loop.now;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 2);
  EXPECT_EQ(loop.releases[CONFIRM], 2);
  EXPECT_LT(loop.now - start, 300u) << "inside a double-press window";
}

TEST(ButtonPressQueue, PressesPlayInOrder) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(BACK, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  for (int i = 0; i < 20 && loop.presses[BACK] == 0; ++i) loop.pass();
  ASSERT_EQ(loop.presses[BACK], 1);
  EXPECT_EQ(loop.presses[CONFIRM], 0);
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
}

TEST(ButtonPressQueue, AFullQueueRefuses) {
  ButtonPressQueue queue;
  for (size_t i = 0; i < ButtonPressQueue::CAPACITY; ++i) EXPECT_TRUE(queue.push(BACK, 60));
  EXPECT_EQ(queue.freeSlots(), 0u);
  EXPECT_FALSE(queue.push(BACK, 60));
  EXPECT_FALSE(ButtonPressQueue().push(8, 60)) << "no such button bit";
}

// A board whose input path never sees the hook: the press is given up, not held forever.
TEST(ButtonPressQueue, AnUncommittedPressIsGivenUp) {
  Loop loop;
  loop.input.ignoresHook = true;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(BACK, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.queue.droppedPresses(), 2u);
  EXPECT_EQ(loop.queue.takenPresses(), 0u);
  EXPECT_EQ(loop.held, 0);
}

// One loop pass blocked for seconds (a command waiting for the render lock) right after the press
// started: the input layer commits it on the next update, so it must not be given up meanwhile.
TEST(ButtonPressQueue, AStalledPassDoesNotDropThePress) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  loop.pass();  // the queue starts holding the button
  // The next pass: one input update sees the button (its debounce starts), then the pass stalls
  // before step() runs.
  loop.now += 10;
  loop.input.update(loop.now, loop.held);
  loop.now += 3000;
  loop.held = loop.queue.step(loop.now, loop.input.currentState);
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.queue.droppedPresses(), 0u);
}

TEST(ButtonPressQueue, NotIdleUntilTheReleaseIsCommitted) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  for (int i = 0; i < 50 && (loop.held != 0 || loop.presses[CONFIRM] == 0); ++i) loop.pass();
  ASSERT_EQ(loop.presses[CONFIRM], 1);
  ASSERT_EQ(loop.held, 0);
  // Released by the queue, but the input layer still reports the button down.
  EXPECT_TRUE(loop.input.currentState & (1u << CONFIRM));
  EXPECT_FALSE(loop.queue.idle());
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.input.currentState, 0);
}

// A real key held over the end of a press: its release is never committed. The next press of that
// button must wait for the key to come up, or the input layer would see no new press.
TEST(ButtonPressQueue, TheNextPressWaitsForAReleaseNeverCommitted) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  for (int i = 0; i < 20 && loop.presses[CONFIRM] == 0; ++i) loop.pass();
  ASSERT_EQ(loop.presses[CONFIRM], 1) << "the first press is committed before the key goes down";
  loop.input.realKeys = 1u << CONFIRM;
  for (int i = 0; i < 150; ++i) loop.pass();  // past the first release's 1 s timeout
  EXPECT_EQ(loop.presses[CONFIRM], 1);
  EXPECT_EQ(loop.held, 0) << "the second press is not held while the key is down";
  loop.input.realKeys = 0;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.presses[CONFIRM], 2) << "the second press has its own press edge";
  EXPECT_EQ(loop.releases[CONFIRM], 2);
  EXPECT_EQ(loop.queue.droppedPresses(), 1u) << "the first press: its release was never taken in";
}

// One press, its release never taken in (a real key held over its end), nothing queued after it.
TEST(ButtonPressQueue, AReleaseNeverTakenInIsGivenUp) {
  Loop loop;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  for (int i = 0; i < 20 && loop.presses[CONFIRM] == 0; ++i) loop.pass();
  ASSERT_EQ(loop.presses[CONFIRM], 1);
  loop.input.realKeys = 1u << CONFIRM;
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.queue.droppedPresses(), 1u);
  EXPECT_EQ(loop.queue.takenPresses(), 1u);
  EXPECT_EQ(loop.releases[CONFIRM], 0);
}

// The same press through CMD:IDLE's decision: the wait must fail, not answer idle.
TEST(ButtonPressQueue, AReleaseNeverTakenInFailsTheIdleThatFollows) {
  Loop loop;
  IdleGate gate;
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  gate.noteEffect(loop.queue.takenPresses());
  ASSERT_TRUE(gate.begin(loop.now, 10000));
  IdleGate::Verdict verdict = IdleGate::Verdict::Waiting;
  for (int i = 0; i < 1100 && verdict == IdleGate::Verdict::Waiting; ++i) {
    loop.pass();
    if (loop.presses[CONFIRM] == 1) loop.input.realKeys = 1u << CONFIRM;  // held from here on
    IdleGate::Sample sample;
    sample.nowMs = loop.now;
    sample.effectHandled = loop.queue.idle();
    sample.droppedPresses = loop.queue.droppedPresses();
    sample.takenPresses = loop.queue.takenPresses();
    verdict = gate.step(sample);
  }
  EXPECT_EQ(verdict, IdleGate::Verdict::PressDropped);
}

// The key never comes up: the press is given up and counted, not reported as played.
TEST(ButtonPressQueue, APressWhoseButtonStaysDownIsGivenUp) {
  Loop loop;
  loop.input.realKeys = 1u << CONFIRM;
  for (int i = 0; i < 3; ++i) loop.pass();  // the key is committed as down
  ASSERT_TRUE(loop.queue.push(CONFIRM, ButtonPressQueue::SHORT_HOLD_MS));
  ASSERT_TRUE(loop.runUntilIdle());
  EXPECT_EQ(loop.queue.droppedPresses(), 1u);
  EXPECT_EQ(loop.presses[CONFIRM], 1) << "only the real key's press";
  EXPECT_EQ(loop.held, 0);
}
