#pragma once
#include <functional>
#include <string>
#include <vector>

#include "../ListSelectionActivity.h"

class ScreenMarginSelectionActivity final : public ListSelectionActivity {
  std::vector<std::string> options;  // Screen margin options

 protected:
  void loadItems() override;  // Called by base class onEnter

 public:
  explicit ScreenMarginSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        const std::function<void()>& onBack);
};
