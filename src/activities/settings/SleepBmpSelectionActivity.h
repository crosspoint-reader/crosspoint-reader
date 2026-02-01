#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class SleepBmpSelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> files;
  void loadFiles();

 protected:
  void loadItems() override;

 public:
  explicit SleepBmpSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                     const std::function<void()>& onBack);
  void onExit() override;
};
