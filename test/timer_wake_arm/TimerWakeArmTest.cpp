// The production sleep function runs against controlled IDF and board boundaries.
#include <cassert>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#define ENABLE_SERIAL_LOG
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_INVALID_ARG = 2;
constexpr int ESP_SLEEP_WAKEUP_TIMER = 4, GPIO_NUM_13 = 13;
constexpr int GPIO_MODE_OUTPUT = 1, OUTPUT = 1, LOW = 0, HIGH = 1;
using gpio_num_t = int;
static constexpr gpio_num_t XTEINK_C3_GPIO13 = GPIO_NUM_13;
static int armResult = ESP_OK, clearResult = ESP_OK, armCalls = 0, clearCalls = 0, latchWrites = 0;
static int latchLevel = -1;
static bool timerArmed = false, serialEnded = false;
static std::vector<int> errors;
static std::vector<std::string> trace;
struct Slept {};
struct Restarted {};

int esp_sleep_enable_timer_wakeup(uint64_t interval) {
  assert(interval == 60000000);
  ++armCalls;
  trace.push_back("arm");
  if (armResult == ESP_OK) timerArmed = true;
  return armResult;
}
int esp_sleep_disable_wakeup_source(int source) {
  assert(source == ESP_SLEEP_WAKEUP_TIMER);
  ++clearCalls;
  trace.push_back("clear");
  if (clearResult == ESP_OK || clearResult == ESP_ERR_INVALID_STATE) timerArmed = false;
  return clearResult;
}
[[noreturn]] void esp_restart() {
  trace.push_back("restart");
  throw Restarted{};
}
void recordError(int code) {
  assert(!serialEnded);
  errors.push_back(code);
  trace.push_back("error");
}
#define LOG_ERR(tag, message, code) recordError(code)
struct Serial {
  void end() {
    serialEnded = true;
    trace.push_back("serial-end");
  }
} logSerial;
struct HalGPIO {
  bool isXteinkDevice() const { return true; }
};
struct HalPowerManager {
  void startDeepSleep(HalGPIO&, uint64_t = 0) const;
};
namespace BoardConfig {
enum class Board { XteinkX4, XteinkX3 };
struct Profile {
  Board board = Board::XteinkX4;
  struct Power {
    int8_t latch0 = -1, latch1 = -1;
  } power;
} ACTIVE;
}  // namespace BoardConfig
void gpio_hold_dis(gpio_num_t) {}
void gpio_set_direction(gpio_num_t, int) {}
void gpio_set_level(gpio_num_t pin, int value) {
  assert(pin == 13);
  ++latchWrites;
  latchLevel = value;
}
void gpio_hold_en(gpio_num_t) {}
void pinMode(int, int) {}
void digitalWrite(int, int) {}
namespace freeink {
struct PowerManager {
  static void powerDownRailsForSleep() { trace.push_back("rails-down"); }
  [[noreturn]] static void deepSleepUntilPowerButton() {
    trace.push_back("sleep");
    throw Slept{};
  }
};
}  // namespace freeink
#include "SleepSource.inc"

static void reset() {
  armResult = clearResult = ESP_OK;
  armCalls = clearCalls = latchWrites = 0;
  latchLevel = -1;
  timerArmed = serialEnded = false;
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX4;
  errors.clear();
  trace.clear();
}
static bool sleep(uint64_t interval) {
  HalGPIO gpio;
  try {
    HalPowerManager{}.startDeepSleep(gpio, interval);
  } catch (const Slept&) {
    return true;
  }
  return false;
}
static bool rejectedRequest() {
  reset();
  armResult = ESP_ERR_INVALID_ARG;
  timerArmed = true;  // A previously configured source must not survive the failed replacement.
  return sleep(60000000) && armCalls == 1 && clearCalls == 1 && !timerArmed &&
         errors == std::vector<int>{ESP_ERR_INVALID_ARG} && trace.front() == "arm" &&
         latchWrites == (SOC_PM_SUPPORT_EXT1_WAKEUP ? 0 : 1) && (SOC_PM_SUPPORT_EXT1_WAKEUP || latchLevel == LOW);
}
int main(int argc, char**) {
  if (argc > 1) return rejectedRequest() ? 0 : 42;
  reset();
  assert(sleep(0) && armCalls == 0 && clearCalls == 0 && errors.empty());
  assert(trace == std::vector<std::string>({"serial-end", "rails-down", "sleep"}));
  assert(latchWrites == (SOC_PM_SUPPORT_EXT1_WAKEUP ? 0 : 1));
  assert(SOC_PM_SUPPORT_EXT1_WAKEUP || latchLevel == LOW);
  reset();
  assert(sleep(60000000) && timerArmed && armCalls == 1 && clearCalls == 0 && errors.empty());
  assert(trace == std::vector<std::string>({"arm", "serial-end", "rails-down", "sleep"}));
  assert(latchWrites == (SOC_PM_SUPPORT_EXT1_WAKEUP ? 0 : 1));
  assert(SOC_PM_SUPPORT_EXT1_WAKEUP || latchLevel == HIGH);
  reset();
  BoardConfig::ACTIVE.board = BoardConfig::Board::XteinkX3;
  assert(sleep(60000000) && timerArmed && armCalls == 1 && clearCalls == 0 && errors.empty());
  assert(latchWrites == (SOC_PM_SUPPORT_EXT1_WAKEUP ? 0 : 1));
  assert(SOC_PM_SUPPORT_EXT1_WAKEUP || latchLevel == LOW);
  assert(rejectedRequest());
  assert(trace == std::vector<std::string>({"arm", "error", "clear", "serial-end", "rails-down", "sleep"}));
  reset();
  armResult = ESP_ERR_INVALID_ARG;
  clearResult = ESP_ERR_INVALID_STATE;
  assert(sleep(60000000) && !timerArmed && errors.size() == 1 && clearCalls == 1);
  reset();
  armResult = ESP_ERR_INVALID_ARG;
  clearResult = 9;
  try {
    sleep(60000000);
    assert(false);
  } catch (const Restarted&) {
  }
  assert(errors == std::vector<int>({ESP_ERR_INVALID_ARG, 9}));
  assert(!serialEnded && latchWrites == 0 && trace.back() == "restart");
  std::cout << "Timer arm failure/cleanup, C3 latch, and EXT1 paths passed\n";
}
