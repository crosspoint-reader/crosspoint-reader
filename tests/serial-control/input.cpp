#include <SerialInput.h>

#include <cassert>
#include <cstdint>

int main() {
  SerialInput input;
  assert(!input.start(7, 80));
  assert(!input.start(1, 0));
  assert(input.start(1, 80));
  assert(!input.start(2, 80));
  assert(input.down() == 0);
  input.beginFrame(100);
  assert(input.pressed() == 2 && input.down() == 2);
  assert(input.pressed() == 2);  // repeated polls retain the frame edge
  input.beginFrame(179);
  assert(input.pressed() == 0 && input.down() == 2);
  input.beginFrame(180);
  assert(input.down() == 0 && input.released() == 2 && input.heldMs(500) == 80);
  input.beginFrame(181);
  assert(!input.active());

  assert(input.start(0, 80));
  input.beginFrame(UINT32_MAX - 39);
  input.beginFrame(40);
  assert(input.released() == 1 && input.heldMs(40) == 80);
  input.cancel();
  assert(!input.down() && !input.released() && !input.pressed());

  assert(input.start(3, 80));
  input.beginFrame(1000);
  input.beginFrame(2000);
  assert(input.released() == 8 && input.heldMs(2000) == 1000);
  input.cancel();
  assert(input.start(3, 1000));
  input.cancel();  // cancelling pending input emits no click
  input.beginFrame(2001);
  assert(!input.active() && !input.pressed() && !input.released());
}
