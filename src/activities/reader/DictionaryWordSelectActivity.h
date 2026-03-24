#pragma once
#include <Epub/Page.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <memory>
#include <string>
#include <vector>

#include "../Activity.h"
#include "util/DictionaryLookupController.h"
#include "util/WordSelectNavigator.h"

class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop,
                                        const std::string& cachePath, const std::string& nextPageFirstWord = "")
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        fontId(fontId),
        marginLeft(marginLeft),
        marginTop(marginTop),
        cachePath(cachePath),
        nextPageFirstWord(nextPageFirstWord),
        controller(renderer, mappedInput, *this) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::unique_ptr<Page> page;
  int fontId;
  int marginLeft;
  int marginTop;
  std::string cachePath;
  std::string nextPageFirstWord;

  WordSelectNavigator navigator;
  DictionaryLookupController controller;

  bool skipLoopDelay() override { return controller.skipLoopDelay(); }

  void extractWords(std::vector<WordSelectNavigator::WordInfo>& words, std::vector<WordSelectNavigator::Row>& rows);
  void mergeHyphenatedWords(std::vector<WordSelectNavigator::WordInfo>& words,
                            std::vector<WordSelectNavigator::Row>& rows);
  void handleNotFound(const std::string& word);

  // Multi-word select state
  bool inMultiSelectMode = false;
  int anchorFlatIndex = -1;
  static constexpr unsigned long LONG_PRESS_MS = 600;

  std::string buildPhraseFromRange(int fromIdx, int toIdx) const;
};
