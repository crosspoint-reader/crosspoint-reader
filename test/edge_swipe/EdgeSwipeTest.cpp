#include <cassert>
#include <iostream>
#include <utility>

#include "util/EdgeSwipe.h"

using namespace edge_swipe;

int main() {
  constexpr float dpi = 254;  // 10 px/mm makes threshold boundaries exact.
  Recognizer r;
  r.begin(5, 799, 480, 800, dpi, 0);
  r.move(15, 790, 10);  // corner: horizontal dominant, so left wins
  assert(r.getState().edge == Edge::Left);
  assert(r.getState().position == 799);
  r.cancel();

  r.begin(100, 799, 480, 800, dpi, 0);
  r.move(110, 790, 20);  // movement along bottom edge rejects the gesture
  r.move(110, 690, 200);
  assert(!r.getState().claimed);
  r.release(250);
  assert(!r.getState().commit);

  r.begin(100, 799, 480, 800, dpi, 0);
  r.move(100, 780, 100);
  assert(r.getState().stage == Stage::Peek);
  r.move(115, 779, 200);
  assert(r.getState().stage == Stage::Peek);
  assert(r.getState().position == 100);  // lateral drift never moves the tab
  r.move(115, 730, 400);
  assert(r.getState().stage == Stage::Peek);
  r.move(115, 729, 600);
  assert(r.getState().stage == Stage::Armed);
  r.move(115, 779, 800);
  assert(r.getState().stage == Stage::Peek);
  r.release(900);
  assert(!r.getState().commit);
  assert(r.getState().claimed);  // cancelled edge release cannot become a tap
  r.nextFrame();
  assert(!r.getState().claimed);

  r.begin(0, 300, 480, 800, dpi, 0);
  r.move(20, 302, 10);
  assert(r.getState().edge == Edge::Left);
  assert(r.getState().stage == Stage::Peek);
  r.move(30, 308, 20);
  r.release(21);
  assert(r.getState().commit);  // flick commits without Armed ever appearing

  for (const float deviceDpi : {Config::DEFAULT_DPI, Config::COMPACT_PANEL_DPI}) {
    r.begin(0, 300, 480, 800, deviceDpi, 0);
    r.move(Config::LOCK_PX - 1, 300, 10);
    assert(!r.getState().claimed && r.getState().stage == Stage::Idle);
    r.move(Config::LOCK_PX, 300, 20);
    assert(r.getState().claimed && r.getState().stage == Stage::Peek);
    r.release(21);
    assert(!r.getState().commit);  // feedback at direction lock still needs 2 mm to commit a flick
  }

  r.begin(0, 300, 480, 800, dpi, 0);
  r.move(30, 300, 20);
  r.move(30, 300, 150);  // waiting before release removes the early velocity
  r.release(180);
  assert(!r.getState().commit);

  for (const auto& dimensions : {std::pair{480, 800}, std::pair{800, 480}}) {
    const auto [w, h] = dimensions;
    r.begin(w - 1, h / 2, w, h, dpi, 0);
    assert(!r.pendingDirection());  // right-edge held input belongs to the activity
    r.move(w - 81, h / 2, 100);
    r.release(150);
    assert(r.getState().edge == Edge::None);
    assert(!r.getState().claimed && !r.getState().commit);
  }

  r.begin(479, 0, 480, 800, dpi, 0);
  r.move(400, 1, 100);
  r.release(150);
  assert(!r.getState().claimed && !r.getState().commit);
  r.begin(479, 0, 480, 800, dpi, 0);
  r.move(479, 70, 100);
  r.release(150);
  assert(r.getState().edge == Edge::Top && r.getState().commit);

  r.begin(0, 300, 480, 800, dpi, 0);
  r.move(70, 300, 300);
  assert(r.getState().stage == Stage::Armed);
  r.move(10, 300, 500);
  assert(r.getState().stage == Stage::Peek);
  r.move(9, 300, 510);
  assert(r.getState().stage == Stage::Idle);
  r.release(600);
  assert(!r.getState().commit);

  r.begin(200, 0, 480, 800, dpi, 0);
  r.move(200, 70, 300);
  r.release(500);
  assert(r.getState().commit);
  r.nextFrame();
  assert(!r.getState().commit);
  assert(r.getState().committed);

  r.begin(200, 0, 480, 800, dpi, 0);
  r.move(200, 70, 300);
  r.cancel();
  assert(!r.getState().tracking && !r.getState().commit && r.getState().stage == Stage::Idle);

  r.begin(240, 400, 480, 800, dpi, 0);
  assert(!r.pendingDirection());
  r.move(440, 400, 30);
  r.release(40);
  assert(!r.getState().claimed && !r.getState().commit);

  r.begin(0, 300, 480, 800, dpi, 0xfffffff0u);
  r.move(40, 300, 0x10u);
  r.release(0x12u);
  assert(r.getState().commit);  // millis wrap
  std::cout << "Edge swipe checks passed\n";
}
