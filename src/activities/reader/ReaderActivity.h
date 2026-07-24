#pragma once
#include <memory>
#include <optional>

#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class Xtc;
class Txt;
struct PerBookReaderSettings;

class ReaderActivity final : public Activity {
  std::string initialBookPath;
  std::optional<ClippingJumpResult> initialClippingJump;
  std::string currentBookPath;  // Track current book path for navigation
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  std::unique_ptr<Epub> loadEpub(const std::string& path, PerBookReaderSettings& globalSettings,
                                 PerBookReaderSettings& bookSettings, bool& settingsWritable);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  std::unique_ptr<Txt> loadTxt(const std::string& path, PerBookReaderSettings& globalSettings,
                               PerBookReaderSettings& bookSettings, bool& settingsWritable);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  static bool isBmpFile(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub, PerBookReaderSettings globalSettings,
                        PerBookReaderSettings bookSettings, bool settingsWritable);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt, PerBookReaderSettings globalSettings,
                       PerBookReaderSettings bookSettings, bool settingsWritable);
  void onGoToBmpViewer(const std::string& path);

  void onGoBack();

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          std::optional<ClippingJumpResult> initialClippingJump = std::nullopt)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        initialClippingJump(std::move(initialClippingJump)) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
