#include <cassert>
#include <cstdint>
#include <cstdio>

#include "HalUsbDfu.h"
uint32_t testRegisters[2]{};
unsigned testWrites = 0, testPinCalls = 0;
void (*testInterrupt)() = nullptr;
bool testFuses[3]{}, shortPulse = false;
void esp_rom_delay_us(unsigned us) {
  assert(us == 12);
  if (shortPulse) testRegisters[1] = 1u << 19;
}
int main() {
  for (unsigned model = 0; model < 3; model++)
    for (unsigned bits = 0; bits < 8; bits++) {
      HalUsbDfu::stop();
      testWrites = testPinCalls = 0;
      for (unsigned f = 0; f < 3; f++) testFuses[f] = bits & (1u << f);
      HalUsbDfu::begin(model == 1, model == 2);
      assert(HalUsbDfu::enabled() == (model != 0 && bits != 0));
      if (!HalUsbDfu::enabled()) {
        assert(!testWrites && !testPinCalls && !testInterrupt);
        continue;
      }
      assert(testInterrupt && testPinCalls == 2);
      testRegisters[1] = 0;
      shortPulse = true;
      testInterrupt();
      assert(!HalUsbDfu::takeHostRequest());
      testRegisters[1] = 0;
      shortPulse = false;
      testInterrupt();
      assert(HalUsbDfu::takeHostRequest());
      assert(!testInterrupt && !HalUsbDfu::takeHostRequest());
      HalUsbDfu::arm();
      assert(testInterrupt);
      HalUsbDfu::stop();
      assert(!testInterrupt);
    }
  puts(
      "PASS: production eFuse gate leaves native USB pins untouched; EOP ignored, sustained host reset dispatched "
      "once, stop/rearm works.");
}
