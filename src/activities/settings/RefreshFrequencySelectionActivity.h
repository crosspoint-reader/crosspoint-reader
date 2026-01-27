#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class RefreshFrequencySelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> options;  // Refresh frequency options

 protected:
  void loadItems() override;  // Called by base class onEnter

 public:
  explicit RefreshFrequencySelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             const std::function<void()>& onBack);
};
