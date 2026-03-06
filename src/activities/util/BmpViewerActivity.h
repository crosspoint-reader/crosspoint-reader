#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"
#include "MappedInputManager.h"

class BmpViewerActivity final : public Activity {
 public:
  BmpViewerActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath);

  void onEnter() override;
  void onExit() override;
  void loop() override;

 private:
  std::string filePath;
  std::vector<std::string> siblingFiles;
  size_t currentIndex = 0;

  void loadSiblingFiles();
  bool selectAdjacentFile(int direction);
  bool setCurrentFileAsSleepImage() const;
  void renderCurrentImage(bool fullRefresh = true) const;
  void renderMessage(const char* message, bool fullRefresh = false) const;
};
