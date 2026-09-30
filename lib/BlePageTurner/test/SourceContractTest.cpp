// The module depends only on the SDK's BleKeyboardHost, ArduinoJson and the C++ library; the
// chip's headers only in SdkRadio.cpp. Any other include (a host's settings, activities,
// renderer, HAL) would tie the module to one host and break its extraction.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <string>

namespace {

namespace fs = std::filesystem;

const std::set<std::string> kOwn = {"BlePageTurner.h", "BlePageTurnerJson.h", "BleKeyBinding.h",
                                    "RadioPolicy.h",   "RadioPort.h",         "Runtime.h"};
const std::set<std::string> kStd = {"atomic", "cstdarg", "cstddef", "cstdint", "cstdio", "cstring"};
const std::set<std::string> kSdkRadioOnly = {"BleKeyboardHost.h", "esp_attr.h", "esp_timer.h", "freertos/FreeRTOS.h",
                                             "freertos/task.h"};

TEST(BlePageTurnerSourceContractTest, IncludesStayInsideTheAllowList) {
  const fs::path root = BLETURNER_ROOT_PATH;
  const std::regex include(R"(^\s*#\s*include\s*[<"]([^>"]+)[>"])");
  std::string outside;
  unsigned files = 0;
  for (const char* dir : {"src", "include"}) {
    for (const auto& entry : fs::directory_iterator(root / dir)) {
      if (!entry.is_regular_file()) continue;
      ++files;
      std::ifstream in(entry.path());
      std::string line;
      while (std::getline(in, line)) {
        std::smatch m;
        if (!std::regex_search(line, m, include)) continue;
        const std::string name = m[1];
        const std::string file = entry.path().filename().string();
        if (kOwn.count(name) || kStd.count(name)) continue;
        if (name == "ArduinoJson.h" && file == "BlePageTurnerJson.h") continue;
        if (kSdkRadioOnly.count(name) && file == "SdkRadio.cpp") continue;
        outside += file + ": " + name + "\n";
      }
    }
  }
  EXPECT_GE(files, 9u) << "the scan found the module";
  EXPECT_EQ(outside, "") << "includes outside the allow-list:\n" << outside;
}

}  // namespace
