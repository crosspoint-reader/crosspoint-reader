#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"

class Epub;

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialEpubPath;
  std::string currentEpubPath;  // Track current book path for navigation
  const std::function<void()> onGoBack;
  static std::unique_ptr<Epub> loadEpub(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void onSelectEpubFile(const std::string& path);
  void onGoToFileSelection(const std::string& fromEpubPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);

 public:
  explicit ReaderActivity(GfxRenderer& renderer, InputManager& inputManager, std::string initialEpubPath,
                          const std::function<void()>& onGoBack)
      : ActivityWithSubactivity("Reader", renderer, inputManager),
        initialEpubPath(std::move(initialEpubPath)),
        onGoBack(onGoBack) {}
  void onEnter() override;
};
