#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class SleepBmpSelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> files;  // Sorted list of valid BMP filenames ("Random" at index 0)
  void loadFiles();  // Load and sort all valid BMP files

 protected:
  void loadItems() override;  // Called by base class onEnter

 public:
  explicit SleepBmpSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onBack);
  void onExit() override;
};

