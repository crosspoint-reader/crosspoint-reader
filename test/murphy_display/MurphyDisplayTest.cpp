// Runs the real Murphy driver against a recording SPI bus, without hardware.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#include "driver/Uc8253MurphyDriver.h"
#include "lut/Uc8253MurphyLuts.h"

namespace {
std::vector<uint8_t> commands;
std::map<uint8_t, std::vector<uint8_t>> planes, luts;
uint8_t currentCommand = 0;
int resets = 0;
void clearBus() { commands.clear(); planes.clear(); luts.clear(); resets = 0; }
bool sent(uint8_t cmd) { return std::find(commands.begin(), commands.end(), cmd) != commands.end(); }
}

namespace freeink {
void EpdBus::reset(uint16_t) { ++resets; }
void EpdBus::cmd(uint8_t c) {
  currentCommand = c;
  commands.push_back(c);
}
void EpdBus::data(uint8_t) {}
void EpdBus::cmdData(uint8_t c, const uint8_t* p, uint16_t n) { cmd(c); luts[c].assign(p,p+n); }
void EpdBus::waitBusy(const char*) {}
EpdBus::~EpdBus() {}
EpdBus::Transaction::Transaction(EpdBus&) {}
EpdBus::Transaction::~Transaction() {}
EpdBus::Transaction EpdBus::beginTxn() { return Transaction(*this); }
void EpdBus::Transaction::writeBytes(const uint8_t* data, uint16_t n) {
  planes[currentCommand].insert(planes[currentCommand].end(), data, data + n);
}
void EpdBus::fillPlane(uint8_t, uint8_t, uint16_t, uint16_t) {}
}

int main() {
  using namespace freeink;
  EpdBus bus;
  Uc8253MurphyDriver driver;
  constexpr size_t bytes = 416 * 240 / 8;
  std::vector<uint8_t> frame(bytes, 0xff), old(bytes, 0);
  driver.begin(bus);
  clearBus();
  driver.display(bus, frame.data(), nullptr, RefreshMode::Fast, true);
  assert(sent(0x12) && planes[0x10] == planes[0x13]);
  assert(planes[0x10].size() == bytes);
  assert(luts[0x20] == std::vector<uint8_t>(MURPHY_LUT_20_DEFAULT, MURPHY_LUT_20_DEFAULT+42));
  // Both destination banks are usable without a previous host image.
  for (int i=0; i<4; ++i) {
    frame[0] ^= 0x80;
    clearBus();
    driver.display(bus, frame.data(), i % 2 ? old.data() : nullptr, RefreshMode::Fast, true);
    assert(planes[0x10] == planes[0x13]);
    assert(planes[0x10].size() == bytes);
    assert(luts[0x20] == std::vector<uint8_t>(MURPHY_LUT_20_FAST, MURPHY_LUT_20_FAST+42));
  }
  clearBus();
  driver.display(bus, frame.data(), nullptr, RefreshMode::Fast, true);
  assert(luts[0x20] == std::vector<uint8_t>(MURPHY_LUT_20_DEFAULT, MURPHY_LUT_20_DEFAULT+42));
  assert(!driver.grayscaleCapabilities().supported());
  clearBus();
  driver.displayGray(bus, frame.data(), false, nullptr, false);
  assert(commands.empty());
  driver.deepSleep(bus);
  clearBus();
  driver.display(bus, frame.data(), nullptr, RefreshMode::Fast, true);
  assert(sent(0x12));
  std::puts("Murphy single-buffer panel tests passed");
}
