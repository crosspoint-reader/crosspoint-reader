#pragma once
#include <memory>

#include "../ActivityWithSubactivity.h"
#include "activities/home/MyLibraryActivity.h"

class Epub;
class Xtc;
class Txt;

class ReaderActivity final : public ActivityWithSubactivity {
  std::string initialBookPath;
  std::string currentBookPath;        // Track current book path for navigation
  MyLibraryActivity::Tab libraryTab;  // Track which tab to return to
  const std::function<void()> onGoBack;
  const std::function<void(const std::string&, MyLibraryActivity::Tab)> onGoToLibrary;
  static std::unique_ptr<Epub> loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);

  static std::string extractFolderPath(const std::string& filePath);
  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          MyLibraryActivity::Tab libraryTab, const std::function<void()>& onGoBack,
                          const std::function<void(const std::string&, MyLibraryActivity::Tab)>& onGoToLibrary)
      : ActivityWithSubactivity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        libraryTab(libraryTab),
        onGoBack(onGoBack),
        onGoToLibrary(onGoToLibrary) {}
  void onEnter() override;
};
